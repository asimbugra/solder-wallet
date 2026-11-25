#include "web.h"
#include "solder.h"
#include "display.h"
#include "web_ui.h"
#include <esp_http_server.h>
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/platform_util.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <base58.h>
#include <atomic>
#include <algorithm>

static DNSServer dns;
static std::atomic<bool> accepting{false};
static String apName;
static bool restarting;
static char setupToken[33];
static uint32_t setupActivity;
static constexpr uint32_t SETUP_IDLE_MS = 15 * 60000UL;

struct Restore
{
    String words, pin;
    unsigned page = 0, pages = 0;
    uint32_t started = 0, changedAt = 0, sampledAt = 0, approvedAt = 0;
    bool rawPressed = false, armed = false, holding = false, approved = false;
};
static Restore restore;

#if SOLDER_OLED
enum class BackupPhase { Idle, Checking, Incorrect, Verified };
struct BackupCheck
{
    BackupPhase phase = BackupPhase::Idle;
    unsigned wordCount = 0, question = 0, positions[3] = {}, selected = 0, correct = 0;
    const char *choices[3] = {};
    uint32_t sampledAt = 0, changedAt = 0;
    bool rawDown = false, stableDown = true, pressing = false;
};
static BackupCheck backupCheck;
#endif

// The HTTP task reads a bounded request before handing it to the Arduino loop.
// All wallet, button and display operations stay on that one loop.
static SemaphoreHandle_t requestReady, responseReady;
static struct
{
    void (*handler)();
    char body[6200]; // accommodates a 1024-byte message even with JSON escaping
    size_t bodyLength;
    char token[33];
    uint32_t expectedId;
    String response;
    int code;
} call;
static StaticJsonDocument<512> in; // parses strings in place in call.body

static void sendJson(int code, const JsonDocument &doc)
{
    call.code = code;
    size_t size = measureJson(doc);
    if (doc.overflowed() || !call.response.reserve(size) || serializeJson(doc, call.response) != size)
    {
        call.code = 500;
        call.response = "{\"error\":\"Response too large\"}";
    }
}

static void fail(int code, const String &message)
{
    StaticJsonDocument<384> d;
    d["error"] = message;
    sendJson(code, d);
}

static bool session(bool touch = true)
{
    if (solder::checkSession(call.token, touch)) return true;
    fail(401, "Session ended, unlock again");
    return false;
}

static void clearSetup()
{
#if SOLDER_OLED
    backupCheck = BackupCheck();
#endif
    if (restore.words.length()) mbedtls_platform_zeroize(restore.words.begin(), restore.words.length());
    if (restore.pin.length()) mbedtls_platform_zeroize(restore.pin.begin(), restore.pin.length());
    restore = Restore();
    mbedtls_platform_zeroize(setupToken, sizeof(setupToken));
    solder::clearPendingMnemonic();
}

static void issueSetupToken()
{
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i)
    {
        setupToken[2 * i] = hex[bytes[i] >> 4];
        setupToken[2 * i + 1] = hex[bytes[i] & 15];
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    setupActivity = millis();
}

static bool setupOwner(bool touch = true)
{
    JsonString supplied = in["setupToken"].as<JsonString>();
    if (setupToken[0] && supplied.size() == 32 &&
        mbedtls_ct_memcmp(supplied.c_str(), setupToken, 32) == 0)
    {
        if (touch) setupActivity = millis();
        return true;
    }
    fail(409, "Backup session changed. Return to the tab that created the words or start again after it expires.");
    return false;
}

static bool setupDisplay()
{
    if (display::available()) return true;
    clearSetup();
    fail(503, "Reconnect the wallet display and start setup again");
    return false;
}

// Nothing works before the factory WiFi password is replaced
static bool passwordChanged()
{
    if (!solder::settings.defaultPassword()) return true;
    fail(428, "Change the WiFi password first");
    return false;
}

static bool decode32(const String &a)
{
    uint8_t buf[40];
    size_t len = sizeof(buf);
    return a.length() >= 32 && a.length() <= 44 && base58Decode(a, buf, len) && len == 32;
}

// "1.25" with 9 decimals -> 1250000000. No floats, so no rounding surprises.
static bool parseAmount(String &text, uint8_t decimals, uint64_t &out)
{
    text.trim();
    uint64_t amount = 0;
    unsigned fractional = 0;
    bool afterPoint = false;
    for (unsigned i = 0; i < text.length(); ++i)
    {
        char c = text[i];
        if ((c == '.' || c == ',') && !afterPoint)
        {
            afterPoint = true;
            continue;
        }
        if (c < '0' || c > '9' || (afterPoint && ++fractional > decimals)) return false;
        unsigned digit = c - '0';
        if (amount > (UINT64_MAX - digit) / 10) return false;
        amount = amount * 10 + digit;
    }
    if (!amount) return false;
    for (unsigned i = fractional; i < decimals; ++i)
    {
        if (amount > UINT64_MAX / 10) return false;
        amount *= 10;
    }
    out = amount;
    // Format the exact base-unit value independently of input padding.
    char buffer[24];
    int pos = sizeof(buffer);
    buffer[--pos] = 0;
    for (unsigned i = 0; amount || i <= decimals; ++i)
    {
        if (decimals && i == decimals) buffer[--pos] = '.';
        buffer[--pos] = '0' + (amount % 10);
        amount /= 10;
    }
    char *end = buffer + sizeof(buffer) - 1;
    if (decimals)
    {
        while (end[-1] == '0') --end;
        if (end[-1] == '.') --end;
        *end = 0;
    }
    text = buffer + pos;
    return true;
}

static void requestJson(JsonObject o)
{
    const Request &r = solder::request;
    o["id"] = r.id;
    o["kind"] = reqKindName(r.kind);
    o["state"] = reqStateName(r.state);
    o["hold"] = r.hold;
    if (r.kind == ReqKind::SendSol || r.kind == ReqKind::SendToken)
    {
        o["to"] = r.to;
        o["amount"] = r.amountText;
        o["symbol"] = r.symbol;
        if (r.mint.length()) o["mint"] = r.mint;
    }
    if (r.kind == ReqKind::SignMessage)
    {
        o["message"] = String((const char *)r.message.data(), r.message.size());
    }
    if (r.signature.length()) o["signature"] = r.signature;
    if (r.tx.length()) o["tx"] = r.tx;
    if (r.error.length()) o["error"] = r.error;
    uint32_t age = millis() - r.createdMs;
    o["expiresIn"] = age < SOLDER_REQUEST_TIMEOUT_MS ? (SOLDER_REQUEST_TIMEOUT_MS - age) / 1000 : 0;
}

static void respondRequest()
{
    DynamicJsonDocument d(2048);
    requestJson(d.createNestedObject("request"));
    sendJson(200, d);
}

// ---------------------------------------------------------------------------
// state, WiFi password, setup
// ---------------------------------------------------------------------------

static void handleState()
{
    DynamicJsonDocument d(1024);
    d["version"] = SOLDER_VERSION;
    d["apName"] = apName;
    d["defaultPassword"] = solder::settings.defaultPassword();
    d["hasWallet"] = solder::store.exists();
    bool owner = solder::checkSession(call.token, false);
    d["unlocked"] = owner;
    d["restarting"] = restarting;
    d["restorePending"] = restore.words.length() != 0;
    d["network"] = solder::settings.network;
    if (owner)
    {
        d["address"] = solder::address();
        d["rpc"] = solder::settings.rpcUrl();
        d["autoLockMin"] = solder::settings.autoLockMin;
    }
    d["screen"] = (bool)SOLDER_OLED;
    d["holdMs"] = SOLDER_HOLD_MS;
    d["timeoutS"] = SOLDER_REQUEST_TIMEOUT_MS / 1000;
    sendJson(200, d);
}

static void handleApPassword()
{
    if ((solder::store.exists() || !solder::settings.defaultPassword()) && !session()) return;
    String pass = in["password"] | "";
    if (pass.length() < 8 || pass.length() > 63 || pass == solder::settings.apPass || pass == "admin123")
    {
        fail(400, "Choose a new WiFi password of 8 to 63 characters");
        return;
    }
    for (unsigned i = 0; i < pass.length(); ++i)
    {
        if (static_cast<uint8_t>(pass[i]) < 32 || static_cast<uint8_t>(pass[i]) > 126)
        {
            fail(400, "Use printable ASCII characters for the WiFi password");
            return;
        }
    }
    if (!solder::settings.setApPassword(pass))
    {
        fail(500, "Could not save the WiFi password");
        return;
    }
    restarting = true;
    solder::lock();
    clearSetup();
    display::text("WiFi password set", "Restarting...");
    StaticJsonDocument<64> d;
    d["restarting"] = true;
    sendJson(200, d);
    solder::restartSoon(1500);
}

static void handleGenerate()
{
    if (!passwordChanged()) return;
    if (solder::store.exists())
    {
        fail(409, "A wallet already exists");
        return;
    }
    if (setupToken[0] && !setupOwner()) return;
    if (!setupDisplay()) return;
    int count = in["words"] | 12;
    if ((in.containsKey("words") && !in["words"].is<int>()) || (count != 12 && count != 24))
    {
        fail(400, "Choose 12 or 24 words");
        return;
    }
#if SOLDER_OLED
    backupCheck = BackupCheck();
#endif
    const String &words = solder::generateMnemonic(count);
    if (!words.length())
    {
        clearSetup();
        fail(500, "Could not generate backup words");
        return;
    }
    issueSetupToken();
    DynamicJsonDocument d(512);
    d["setupToken"] = setupToken;
#if SOLDER_OLED
    // The words never leave the device, they are shown on its screen
    display::words(words, 0);
    d["onScreen"] = true;
    d["count"] = count;
#else
    d["words"] = words.c_str();
#endif
    sendJson(200, d);
}

#if SOLDER_OLED
// Refer to words in the pending phrase without making extra copies.
static const char *wordAtPosition(const char *words, unsigned position)
{
    while (position--) words = strchr(words, ' ') + 1;
    return words;
}

static bool sameBackupWord(const char *a, const char *b)
{
    const size_t length = strcspn(a, " ");
    return strncmp(a, b, length) == 0 && (b[length] == ' ' || b[length] == '\0');
}

static void drawBackupQuestion()
{
    display::backupQuestion(backupCheck.question + 1, backupCheck.positions[backupCheck.question] + 1,
                            backupCheck.choices, backupCheck.selected);
}

static void prepareBackupQuestion()
{
    const char *words = solder::pendingMnemonic().c_str();
    const char *options[3] = {wordAtPosition(words, backupCheck.positions[backupCheck.question])};
    // Use other words on the paper so the position matters. Valid phrases
    // with fewer than three distinct words need extra BIP39 choices.
    static const char *const extra[] = {"ability", "cactus", "yellow"};
    const unsigned count = backupCheck.wordCount;
    const unsigned start = esp_random() % count;
    unsigned filled = 1;
    for (unsigned n = 0; filled < 3 && n < count + 3; ++n)
    {
        const char *choice = n < count ? wordAtPosition(words, (start + n) % count) : extra[n - count];
        if (sameBackupWord(choice, options[0]) || (filled == 2 && sameBackupWord(choice, options[1]))) continue;
        options[filled++] = choice;
    }
    backupCheck.correct = esp_random() % 3;
    backupCheck.selected = 0;
    for (unsigned i = 0; i < 3; ++i)
        backupCheck.choices[(backupCheck.correct + i) % 3] = options[i];
    drawBackupQuestion();
}

static void startBackupCheck()
{
    backupCheck = BackupCheck();
    const String &words = solder::pendingMnemonic();
    const unsigned count = 1 + std::count(words.c_str(), words.c_str() + words.length(), ' ');
    backupCheck.wordCount = count;
    unsigned positions[24];
    for (unsigned i = 0; i < count; ++i) positions[i] = i;
    for (unsigned i = 0; i < 3; ++i)
    {
        const unsigned pick = i + esp_random() % (count - i);
        std::swap(positions[i], positions[pick]);
        backupCheck.positions[i] = positions[i];
    }
    backupCheck.phase = BackupPhase::Checking;
    backupCheck.sampledAt = backupCheck.changedAt = setupActivity = millis();
    backupCheck.rawDown = digitalRead(SOLDER_BUTTON_PIN) == LOW;
    prepareBackupQuestion();
}

static void handleBackupCheck()
{
    if (!setupOwner(false) || !setupDisplay()) return;
    if (solder::store.exists() || !solder::pendingMnemonic().length())
    {
        fail(409, "No generated backup to check");
        return;
    }
    const char *action = in["action"] | "";
    if (!strcmp(action, "start"))
    {
        if (backupCheck.phase == BackupPhase::Idle || backupCheck.phase == BackupPhase::Incorrect) startBackupCheck();
    }
    else if (strcmp(action, "status")) { fail(400, "Choose start or status"); return; }
    StaticJsonDocument<64> d;
    switch (backupCheck.phase)
    {
    case BackupPhase::Idle:      d["state"] = "idle"; break;
    case BackupPhase::Checking:  d["state"] = "checking"; break;
    case BackupPhase::Incorrect: d["state"] = "incorrect"; break;
    case BackupPhase::Verified:  d["state"] = "verified"; break;
    }
    sendJson(200, d);
}

static void reviewBackupCheck(uint32_t now)
{
    if (backupCheck.phase != BackupPhase::Checking) return;
    const bool down = digitalRead(SOLDER_BUTTON_PIN) == LOW;
    // A button already held when the check starts, or through a gap in
    // sampling, needs a stable release before it can select anything.
    if (now - backupCheck.sampledAt > 250)
    {
        backupCheck.stableDown = true;
        backupCheck.pressing = false;
        backupCheck.changedAt = now;
    }
    backupCheck.sampledAt = now;
    if (down != backupCheck.rawDown)
    {
        backupCheck.rawDown = down;
        backupCheck.changedAt = now;
    }
    const uint32_t elapsed = now - backupCheck.changedAt;
    if (elapsed < 40) return;
    if (down != backupCheck.stableDown)
    {
        backupCheck.stableDown = down;
        if (!down && backupCheck.pressing)
        {
            if (!display::available()) { clearSetup(); return; }
            backupCheck.selected = (backupCheck.selected + 1) % 3;
            setupActivity = now;
            drawBackupQuestion();
        }
        backupCheck.pressing = down;
    }
    if (!down || !backupCheck.pressing || elapsed < SOLDER_HOLD_MS) return;
    backupCheck.pressing = false;
    if (!display::available()) { clearSetup(); return; }
    setupActivity = now;
    if (backupCheck.selected != backupCheck.correct)
    {
        backupCheck.phase = BackupPhase::Incorrect;
        display::text("Check your paper", "Retry from phone");
    }
    else if (++backupCheck.question == 3)
    {
        backupCheck.phase = BackupPhase::Verified;
        display::text("Backup checked", "Set PIN on phone");
    }
    else prepareBackupQuestion();
}
#endif

static void handleWordsPage()
{
    if (!passwordChanged() || !setupOwner()) return;
    if (!setupDisplay()) return;
    if (solder::store.exists() || !solder::pendingMnemonic().length())
    {
        fail(409, "No backup words to show");
        return;
    }
#if SOLDER_OLED
    if (backupCheck.phase == BackupPhase::Checking) { fail(409, "Finish the backup check on Solder"); return; }
#endif
    const String &words = solder::pendingMnemonic();
    const unsigned pages = (1 + std::count(words.c_str(), words.c_str() + words.length(), ' ')) / 6;
    if (!in["page"].is<unsigned>() || in["page"].as<unsigned>() >= pages)
    {
        fail(400, "Invalid backup page");
        return;
    }
    display::words(words, in["page"].as<int>());
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

static void handleCancelSetup()
{
    if (setupToken[0])
    {
        if (!setupOwner()) return;
        clearSetup();
        display::idle("", true);
    }
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

static void pendingRestore()
{
    StaticJsonDocument<96> d;
    d["pending"] = true;
    d["setupToken"] = static_cast<const char *>(setupToken);
    sendJson(202, d);
}

static bool stageRestore(const char *input, const char *pin)
{
    if (strlen(pin) != 6 || strspn(pin, "0123456789") != 6 || strlen(input) > 512)
    {
        fail(400, "Enter your backup words and a six-digit PIN");
        return false;
    }
    if (!setupDisplay()) return false;
    // Normalize exactly the string shown on each page and later passed to BIP39.
    bool between = true;
    unsigned count = 0;
    if (!restore.words.reserve(strlen(input))) { fail(500, "Not enough memory"); return false; }
    for (const char *p = input; *p; ++p)
    {
        char c = *p;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { between = true; continue; }
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c < 'a' || c > 'z')
        {
            clearSetup();
            fail(400, "Check the backup words, spelling and order");
            return false;
        }
        if (between)
        {
            if (count++) restore.words += ' ';
            between = false;
        }
        restore.words += c;
    }
    if ((count != 12 && count != 24) || !Mnemonic::isValid(restore.words))
    {
        clearSetup();
        fail(400, "Check the backup words, spelling and order");
        return false;
    }
    restore.pin = pin;
    if (restore.pin.length() != 6)
    {
        clearSetup();
        fail(500, "Not enough memory to stage the restore");
        return false;
    }
    restore.pages = SOLDER_OLED ? count / 6 : 0;
    restore.started = restore.changedAt = restore.sampledAt = millis();
    restore.rawPressed = digitalRead(SOLDER_BUTTON_PIN) == LOW;
    issueSetupToken();
    if (restore.pages) display::words(restore.words, 0);
    else display::text("Restore wallet", "Hold BOOT to confirm");
    return true;
}

static void handleFinish()
{
    if (solder::store.exists())
    {
        fail(409, "A wallet already exists");
        return;
    }
    const char *explicitWords = in["mnemonic"] | "";
    const char *enteredPin = in["pin"] | "";
    if (!setupToken[0] && *explicitWords)
    {
        if (stageRestore(explicitWords, enteredPin)) pendingRestore();
        return;
    }
    if (!setupOwner()) return;
    const bool importing = restore.words.length() != 0;
    if (*explicitWords || (importing && *enteredPin))
    {
        fail(409, "Cancel this setup before changing the backup or PIN");
        return;
    }
    if (importing && !restore.approved) { pendingRestore(); return; }
#if SOLDER_OLED
    if (!importing && backupCheck.phase != BackupPhase::Verified)
    {
        fail(409, "Check the paper backup on Solder before setting a PIN");
        return;
    }
#endif
    if (!setupDisplay()) return;
    String pin = importing ? restore.pin : String(enteredPin);
    const String &words = importing ? restore.words : solder::pendingMnemonic();
    String error, token;
    if (!solder::finishSetup(words, pin, error))
    {
        if (importing) { clearSetup(); display::idle("", true); }
        mbedtls_platform_zeroize(pin.begin(), pin.length());
        fail(400, error);
        return;
    }
    bool unlocked = solder::unlock(pin, token, error);
    mbedtls_platform_zeroize(pin.begin(), pin.length());
    clearSetup();
    if (!unlocked)
    {
        fail(500, "Wallet saved. Unlock it with your PIN.");
        return;
    }
    StaticJsonDocument<192> d;
    d["token"] = token.c_str();
    d["address"] = solder::address();
    sendJson(200, d);
}

// Each tap requires a stable release. Holding through the last page cannot
// count toward approval. A browser may only collect the resulting grant.
static void reviewRestore(uint32_t now)
{
    if (restore.approved) return;
    bool pressed = digitalRead(SOLDER_BUTTON_PIN) == LOW;
    if (now - restore.sampledAt > 250)
    {
        restore.armed = restore.holding = false;
        restore.changedAt = now;
    }
    restore.sampledAt = now;
    if (pressed != restore.rawPressed)
    {
        restore.changedAt = now;
        restore.rawPressed = pressed;
        if (!pressed) restore.holding = false;
    }
    if (now - restore.changedAt < 40) return;
    if (!pressed)
    {
        restore.armed = true;
        return;
    }
    if (restore.armed)
    {
        restore.armed = false;
        if (!display::available()) { clearSetup(); return; }
        if (restore.page < restore.pages)
        {
            if (++restore.page < restore.pages) display::words(restore.words, restore.page);
            else display::text("Restore these words?", "Hold BOOT to confirm");
        }
        else restore.holding = true;
    }
    if (restore.holding && now - restore.changedAt >= SOLDER_HOLD_MS)
    {
        if (!display::available()) { clearSetup(); return; }
        restore.approved = true;
        restore.approvedAt = now;
        display::text("Restore approved");
    }
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

static void handleUnlock()
{
    // Legacy versions accepted longer, non-numeric PINs. Keep those usable;
    // malformed requests must not consume an owner's remaining attempts.
    JsonString pin = in["pin"].as<JsonString>();
    if (pin.size() < 6 || pin.size() > 1024)
    {
        fail(400, "Enter the wallet PIN");
        return;
    }
    String token, error;
    if (!solder::unlock(pin.c_str(), token, error, in["pinClient"] | ""))
    {
        fail(403, error);
        return;
    }
    StaticJsonDocument<192> d;
    d["token"] = token;
    d["address"] = solder::address();
    sendJson(200, d);
}

static void handleLock()
{
    if (!session()) return;
    solder::lock();
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

// ---------------------------------------------------------------------------
// requests that need the button
// ---------------------------------------------------------------------------

static bool readTransfer(Request &r)
{
    if (!session()) return false;
    r.to = in["to"] | "";
    r.to.trim();
    r.amountText = in["amount"] | "";
    if (in.containsKey("mint") && !in["mint"].is<const char *>())
    {
        fail(400, "Invalid token mint");
        return false;
    }
    r.mint = in["mint"] | "";
    r.blockhash = in["blockhash"] | "";
    if (!decode32(r.to))
    {
        fail(400, "That is not a Solana address");
        return false;
    }
    if (r.to == solder::address())
    {
        fail(400, "This is your own address");
        return false;
    }
    if (!decode32(r.blockhash))
    {
        fail(400, "Missing recent blockhash, is your phone online?");
        return false;
    }

    if (r.mint.length())
    {
        // decimals and token program come from the browser; TransferChecked makes the
        // chain reject the transaction if they don't match the mint
        String prog = in["program"] | INFRATIC_TOKEN_PROGRAM_ID;
        if (!in["decimals"].is<uint8_t>() || in["decimals"].as<unsigned>() > 18 ||
            (in.containsKey("program") && !in["program"].is<const char *>()) ||
            !decode32(r.mint) || (prog != INFRATIC_TOKEN_PROGRAM_ID && prog != INFRATIC_TOKEN_2022_PROGRAM_ID))
        {
            fail(400, "Unknown token");
            return false;
        }
        r.kind = ReqKind::SendToken;
        r.decimals = in["decimals"] | 0;
        r.tokenProgram = prog;
        r.symbol = r.mint.substring(0, 4);
    }
    else
    {
        r.kind = ReqKind::SendSol;
        r.decimals = 9;
        r.symbol = "SOL";
    }
    if (!parseAmount(r.amountText, r.decimals, r.amount))
    {
        fail(400, "Enter an amount with at most " + String(r.decimals) + " decimals");
        return false;
    }
    return true;
}

static void handleSendPreview()
{
    Request r;
    if (!readTransfer(r)) return;
    TransferPreview preview;
    if (!solder::previewTransfer(r, preview))
    {
        fail(500, "Could not build the transfer preview");
        return;
    }
    char units[21];
    snprintf(units, sizeof(units), "%llu", static_cast<unsigned long long>(r.amount));
    StaticJsonDocument<192> d;
    d["amountUnits"] = static_cast<const char *>(units);
    d["message"] = preview.message.c_str();
    if (preview.destinationAccount.length()) d["destinationAccount"] = preview.destinationAccount.c_str();
    if (preview.sizeTransaction.length()) d["sizeTransaction"] = preview.sizeTransaction.c_str();
    sendJson(200, d);
}

static void handleSend()
{
    Request r;
    if (!readTransfer(r)) return;
    String error;
    if (!solder::startRequest(r, error))
    {
        fail(409, error);
        return;
    }
    respondRequest();
}

static bool signableText(const uint8_t *bytes, size_t length)
{
    // A legacy transaction begins with its required signature count, and v0
    // begins with 0x80. Neither can be the start of this UTF-8 text format.
    if (!length || length > 1024 || bytes[0] < 0x20) return false;
    for (size_t i = 0; i < length;)
    {
        uint8_t lead = bytes[i++];
        if (lead < 0x80)
        {
            if ((lead < 0x20 && lead != '\n' && lead != '\r' && lead != '\t') || lead == 0x7f) return false;
            continue;
        }
        if (lead < 0xc2 || lead > 0xf4) return false;
        unsigned count = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (count > length - i) return false;
        uint8_t second = bytes[i];
        if ((lead == 0xc2 && second < 0xa0) || (lead == 0xe0 && second < 0xa0) ||
            (lead == 0xed && second >= 0xa0) || (lead == 0xf0 && second < 0x90) ||
            (lead == 0xf4 && second >= 0x90)) return false;
        for (unsigned j = 0; j < count; ++j)
            if ((bytes[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}

static void handleSign()
{
    if (!session()) return;
    JsonString msg = in["message"].as<JsonString>();
    auto bytes = reinterpret_cast<const uint8_t *>(msg.c_str());
    if (!signableText(bytes, msg.size()))
    {
        fail(400, "Use UTF-8 text up to 1024 bytes, starting with a printable character");
        return;
    }
    Request r;
    r.kind = ReqKind::SignMessage;
    r.message.assign(bytes, bytes + msg.size());
    String error;
    if (!solder::startRequest(r, error))
    {
        fail(409, error);
        return;
    }
    respondRequest();
}

static void handleWipe()
{
    if (!session()) return;
    Request r;
    r.kind = ReqKind::Wipe;
    String error;
    if (!solder::startRequest(r, error))
    {
        fail(409, error);
        return;
    }
    respondRequest();
}

static void handleRequest()
{
    // Only the empty, completed erase receipt remains public after a wipe.
    bool erased = !solder::store.exists() && solder::request.kind == ReqKind::Wipe &&
                  solder::request.state == ReqState::Done;
    if (!erased && !session(false)) return;
    if (call.expectedId && solder::request.id != call.expectedId)
    {
        fail(409, "The request changed");
        return;
    }
    if (solder::request.state == ReqState::None)
    {
        fail(404, "No request");
        return;
    }
    respondRequest();
}

static void handleReject()
{
    if (!session()) return;
    if (!in["id"].is<uint32_t>() || in["id"].as<uint32_t>() != solder::request.id)
    {
        fail(409, "The request changed");
        return;
    }
    solder::rejectRequest();
    respondRequest();
}

static void handleNetwork()
{
    if (!session()) return;
    String net = in["network"] | "";
    String rpc = in["rpc"] | "";
    if (net != "devnet" && net != "mainnet" && net != "custom")
    {
        fail(400, "Unknown network");
        return;
    }
    if (net == "custom" && !Settings::validCustomRpc(rpc))
    {
        fail(400, "Enter a valid HTTPS RPC address");
        return;
    }
    uint8_t minutes = solder::settings.autoLockMin;
    if (in.containsKey("autoLockMin"))
    {
        minutes = in["autoLockMin"].as<uint8_t>();
        if (!in["autoLockMin"].is<uint8_t>() || (minutes != 0 && minutes != 1 && minutes != 5 && minutes != 15))
        {
            fail(400, "Choose a supported auto lock time");
            return;
        }
    }
    if (!solder::settings.setNetwork(net, net == "custom" ? rpc : String(""), minutes))
    {
        fail(500, "Could not save settings");
        return;
    }
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

// ---------------------------------------------------------------------------

struct Route
{
    const char *path;
    httpd_method_t method;
    void (*handler)();
};

static const Route routes[] = {
    {"/api/state", HTTP_GET, handleState},
    {"/api/ap-password", HTTP_POST, handleApPassword},
    {"/api/setup/generate", HTTP_POST, handleGenerate},
    {"/api/setup/words", HTTP_POST, handleWordsPage},
#if SOLDER_OLED
    {"/api/setup/check", HTTP_POST, handleBackupCheck},
#endif
    {"/api/setup/finish", HTTP_POST, handleFinish},
    {"/api/setup/cancel", HTTP_POST, handleCancelSetup},
    {"/api/unlock", HTTP_POST, handleUnlock},
    {"/api/lock", HTTP_POST, handleLock},
    {"/api/send/preview", HTTP_POST, handleSendPreview},
    {"/api/send", HTTP_POST, handleSend},
    {"/api/sign", HTTP_POST, handleSign},
    {"/api/wipe", HTTP_POST, handleWipe},
    {"/api/request", HTTP_GET, handleRequest},
    {"/api/request/reject", HTTP_POST, handleReject},
    {"/api/network", HTTP_POST, handleNetwork},
};

static const char *httpStatus(int code)
{
    switch (code)
    {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 408: return "408 Request Timeout";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 428: return "428 Precondition Required";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

// The socket's inactivity timeout alone can be kept alive one byte at a time.
// Limit the whole header read as well, even before IDF dispatches a route.
struct ReadWindow { uint32_t start; bool active; };

static int boundedRead(httpd_handle_t server, int socket, char *buf, size_t size, int flags)
{
    auto *window = static_cast<ReadWindow *>(httpd_sess_get_transport_ctx(server, socket));
    if (!window) return HTTPD_SOCK_ERR_FAIL;
    if (!window->active)
    {
        window->start = millis();
        window->active = true;
    }
    if (millis() - window->start >= 3000) return HTTPD_SOCK_ERR_FAIL;
    int count = recv(socket, buf, size, flags);
    return count < 0 ? HTTPD_SOCK_ERR_FAIL : count;
}

static esp_err_t openSocket(httpd_handle_t server, int socket)
{
    if (!accepting.load()) return ESP_FAIL;
    auto *window = static_cast<ReadWindow *>(calloc(1, sizeof(ReadWindow)));
    if (!window) return ESP_ERR_NO_MEM;
    httpd_sess_set_transport_ctx(server, socket, window, free);
    return httpd_sess_set_recv_override(server, socket, boundedRead);
}

static void endRead(httpd_req_t *req)
{
    auto *window = static_cast<ReadWindow *>(httpd_sess_get_transport_ctx(req->handle, httpd_req_to_sockfd(req)));
    if (window) window->active = false;
}

static esp_err_t reply(httpd_req_t *req, int code, const char *json)
{
    endRead(req);
    httpd_resp_set_status(req, httpStatus(code));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(req, json);
}

static bool allowedHost(httpd_req_t *req, String &host)
{
    char buffer[64];
    if (httpd_req_get_hdr_value_str(req, "Host", buffer, sizeof(buffer)) != ESP_OK) return false;
    host = buffer;
    host.toLowerCase();
    if (host.endsWith(":80")) host.remove(host.length() - 3);
    return host == WiFi.softAPIP().toString() || host == SOLDER_HOSTNAME ".local";
}

// These checks run before reading any body or touching wallet state. Returning
// an error closes the socket, so IDF won't drain an oversized or incomplete body.
static esp_err_t handleApi(httpd_req_t *req)
{
    char header[96];
    String host;
    if (!allowedHost(req, host))
    {
        reply(req, 403, "{\"error\":\"Open the wallet at its local address\"}");
        return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_len(req, "Origin"))
    {
        if (httpd_req_get_hdr_value_str(req, "Origin", header, sizeof(header)) != ESP_OK ||
            (String(header) != "http://" + host && String(header) != "http://" + host + ":80"))
        {
            reply(req, 403, "{\"error\":\"Cross-origin request denied\"}");
            return ESP_FAIL;
        }
    }

    const Route &route = *static_cast<const Route *>(req->user_ctx);
    call.expectedId = 0;
    if (httpd_req_get_url_query_len(req))
    {
        char query[24];
        if (route.handler != handleRequest || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            strncmp(query, "id=", 3) != 0 || !query[3])
        {
            reply(req, 400, "{\"error\":\"Invalid request ID\"}");
            return ESP_FAIL;
        }
        uint32_t id = 0;
        for (const char *p = query + 3; *p; ++p)
        {
            if (*p < '0' || *p > '9' || id > (UINT32_MAX - unsigned(*p - '0')) / 10)
            {
                reply(req, 400, "{\"error\":\"Invalid request ID\"}");
                return ESP_FAIL;
            }
            id = id * 10 + unsigned(*p - '0');
        }
        if (!id)
        {
            reply(req, 400, "{\"error\":\"Invalid request ID\"}");
            return ESP_FAIL;
        }
        call.expectedId = id;
    }
    if (req->content_len >= sizeof(call.body) || (route.method == HTTP_GET && req->content_len))
    {
        reply(req, 413, "{\"error\":\"Request too large\"}");
        return ESP_FAIL;
    }
    if (route.method == HTTP_POST)
    {
        if (httpd_req_get_hdr_value_str(req, "Content-Type", header, sizeof(header)) != ESP_OK)
        {
            reply(req, 415, "{\"error\":\"Send JSON\"}");
            return ESP_FAIL;
        }
        String mediaType(header);
        int parameters = mediaType.indexOf(';');
        if (parameters >= 0) mediaType.remove(parameters);
        mediaType.trim();
        if (!mediaType.equalsIgnoreCase("application/json"))
        {
            reply(req, 415, "{\"error\":\"Send JSON\"}");
            return ESP_FAIL;
        }
        if (!req->content_len)
        {
            reply(req, 400, "{\"error\":\"Missing JSON object\"}");
            return ESP_FAIL;
        }
    }

    size_t received = 0;
    uint32_t started = millis();
    while (received < req->content_len)
    {
        int count = httpd_req_recv(req, call.body + received, req->content_len - received);
        if (count > 0) received += static_cast<size_t>(count);
        if (count <= 0 || millis() - started > 3000)
        {
            mbedtls_platform_zeroize(call.body, received);
            reply(req, 408, "{\"error\":\"Request timed out\"}");
            return ESP_FAIL;
        }
    }
    call.bodyLength = received;
    call.body[received] = 0;
    call.token[0] = 0;
    if (httpd_req_get_hdr_value_str(req, "X-Solder-Session", call.token, sizeof(call.token)) != ESP_OK)
        call.token[0] = 0;
    call.handler = route.handler;
    xSemaphoreGive(requestReady);
    xSemaphoreTake(responseReady, portMAX_DELAY);
    esp_err_t result = reply(req, call.code, call.response.c_str());
    if (call.response.length())
        mbedtls_platform_zeroize(const_cast<char *>(call.response.c_str()), call.response.length());
    call.response.clear();
    return result;
}

static esp_err_t captive(httpd_req_t *req)
{
    if (req->content_len)
    {
        reply(req, 413, "{\"error\":\"Unexpected body\"}");
        return ESP_FAIL;
    }
    endRead(req);
    String location = "http://" + WiFi.softAPIP().toString() + "/";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t indexPage(httpd_req_t *req)
{
    if (req->content_len)
    {
        reply(req, 413, "{\"error\":\"Unexpected body\"}");
        return ESP_FAIL;
    }
    endRead(req);
    String host;
    if (!allowedHost(req, host)) return captive(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy", WEB_UI_CSP);
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, reinterpret_cast<const char *>(WEB_UI_GZ), WEB_UI_GZ_LEN);
}

static esp_err_t notFound(httpd_req_t *req, httpd_err_code_t)
{
    if (req->method == HTTP_GET && strncmp(req->uri, "/api/", 5) != 0) return captive(req);
    reply(req, 404, "{\"error\":\"Unknown endpoint\"}");
    return ESP_FAIL;
}

static void dispatch()
{
    if (restarting && call.handler != handleState)
    {
        fail(503, "Restarting with the new WiFi password");
        return;
    }
    if (call.handler != handleState && call.handler != handleApPassword &&
        call.handler != handleUnlock && call.handler != handleLock && !passwordChanged()) return;
    if (restore.words.length() && call.handler != handleFinish && call.handler != handleState &&
        call.handler != handleCancelSetup)
    {
        fail(409, "Finish or cancel the pending restore");
        return;
    }
    if (call.bodyLength)
    {
        if (memchr(call.body, 0, call.bodyLength) ||
            deserializeJson(in, call.body, call.bodyLength, DeserializationOption::NestingLimit(2)) ||
            !in.is<JsonObject>())
        {
            fail(400, "Send a JSON object");
            return;
        }
        for (JsonPair pair : in.as<JsonObject>())
        {
            JsonVariant value = pair.value();
            JsonString str = value.as<JsonString>();
            if (value.is<JsonObject>() || value.is<JsonArray>() ||
                pair.key().size() != strlen(pair.key().c_str()) ||
                (!str.isNull() && str.size() != strlen(str.c_str())))
            {
                fail(400, "Invalid field");
                return;
            }
        }
    }
    call.handler();
}

namespace web
{
    bool begin(const String &ap)
    {
        apName = ap;
        requestReady = xSemaphoreCreateBinary();
        responseReady = xSemaphoreCreateBinary();
        httpd_handle_t server = nullptr;
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]) + 1;
        config.max_resp_headers = 8;
        config.max_req_hdr_len = 4096;
        config.stack_size = 6144;
        config.open_fn = openSocket;
        config.recv_wait_timeout = 2;
        config.send_wait_timeout = 2;
        config.lru_purge_enable = true;
        if (!requestReady || !responseReady || httpd_start(&server, &config) != ESP_OK)
        {
            return false;
        }
        httpd_uri_t page = {};
        page.uri = "/";
        page.method = HTTP_GET;
        page.handler = indexPage;
        bool ready = httpd_register_uri_handler(server, &page) == ESP_OK &&
                     httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound) == ESP_OK;
        for (const auto &route : routes)
        {
            httpd_uri_t api = {};
            api.uri = route.path;
            api.method = route.method;
            api.handler = handleApi;
            api.user_ctx = const_cast<Route *>(&route);
            ready = ready && httpd_register_uri_handler(server, &api) == ESP_OK;
        }
        if (!ready)
        {
            httpd_stop(server);
            return false;
        }
        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(53, "*", WiFi.softAPIP());
        accepting.store(true);
        return true;
    }

    void loop()
    {
        dns.processNextRequest();
        uint32_t now = millis();
        bool restoreExpired = restore.words.length() &&
                              (now - restore.started >= SETUP_IDLE_MS ||
                               (restore.approved && now - restore.approvedAt >= 30000));
        if (setupToken[0] && (now - setupActivity >= SETUP_IDLE_MS || restoreExpired))
        {
            clearSetup();
            display::idle("", true);
        }
        if (restore.words.length()) reviewRestore(now);
#if SOLDER_OLED
        reviewBackupCheck(now);
#endif
        if (requestReady && xSemaphoreTake(requestReady, 0) == pdTRUE)
        {
            dispatch();
            in.clear();
            mbedtls_platform_zeroize(call.body, call.bodyLength);
            mbedtls_platform_zeroize(call.token, sizeof(call.token));
            xSemaphoreGive(responseReady);
        }
    }
}
