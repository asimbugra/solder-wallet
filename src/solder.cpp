#include "solder.h"
#include "display.h"
#include <Preferences.h>
#include <nvs.h>
#include <mbedtls/platform_util.h>
#include <lwip/sockets.h>
#include <base58.h>

const char *reqKindName(ReqKind k)
{
    switch (k)
    {
    case ReqKind::SendSol: return "send";
    case ReqKind::SendToken: return "send-token";
    case ReqKind::SignMessage: return "sign-message";
    case ReqKind::Wipe: return "wipe";
    }
    return "";
}

const char *reqStateName(ReqState s)
{
    switch (s)
    {
    case ReqState::None: return "none";
    case ReqState::Waiting: return "waiting";
    case ReqState::Done: return "done";
    case ReqState::Rejected: return "rejected";
    case ReqState::Failed: return "failed";
    case ReqState::Expired: return "expired";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

namespace
{
    constexpr char PREFS_NS[] = "solder_cfg";
    // Name and key of the existing slot in the pinned Infratic KeyStore format.
    constexpr char WALLET_NS[] = "infratic_ks";
    constexpr char WALLET_KEY[] = "wallet";

    struct Nvs
    {
        nvs_handle_t handle = 0;
        esp_err_t status;
        Nvs(const char *name, nvs_open_mode_t mode) : status(nvs_open(name, mode, &handle)) {}
        ~Nvs() { if (status == ESP_OK) nvs_close(handle); }
    };

    void clearString(String &s)
    {
        if (s.length()) mbedtls_platform_zeroize(const_cast<char *>(s.c_str()), s.length());
        s = "";
    }

    bool validLockTime(uint8_t minutes)
    {
        return minutes == 0 || minutes == 1 || minutes == 5 || minutes == 15;
    }

    // Commit all three network settings as one NVS value.
    struct NetworkPrefs
    {
        uint8_t minutes;
        char network[8];
        char rpc[257];
    };

    bool validNetwork(const String &net, const String &rpc)
    {
        if (net == "devnet" || net == "mainnet") return true;
        return net == "custom" && Settings::validCustomRpc(rpc);
    }
}

bool Settings::validCustomRpc(const String &rpc)
{
    if (!rpc.startsWith("https://") || rpc.length() <= 8 || rpc.length() > 256) return false;
    for (unsigned i = 8; i < rpc.length(); ++i)
        if (uint8_t(rpc[i]) <= 32 || uint8_t(rpc[i]) >= 127 || rpc[i] == '\\' || rpc[i] == '#') return false;
    const char *host = rpc.c_str() + 8;
    const size_t authority = strcspn(host, "/?");
    if (!authority || memchr(host, '@', authority)) return false;
    size_t hostEnd = 0;
    if (host[0] == '[')
    {
        const char *end = static_cast<const char *>(memchr(host, ']', authority));
        if (!end) return false;
        struct in6_addr address;
        if (inet_pton(AF_INET6, String(host + 1, end - host - 1).c_str(), &address) != 1) return false;
        hostEnd = end - host + 1;
    }
    else
    {
        size_t label = 0;
        for (; hostEnd < authority && host[hostEnd] != ':'; ++hostEnd)
        {
            char c = host[hostEnd];
            if (c == '.')
            {
                if (!label || host[hostEnd - 1] == '-') return false;
                label = 0;
                continue;
            }
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                !(c >= '0' && c <= '9') && !(label && c == '-')) return false;
            if (++label > 63) return false;
        }
        if (!label || host[hostEnd - 1] == '-') return false;
    }
    if (hostEnd == authority) return true;
    if (host[hostEnd] != ':' || authority - hostEnd > 6) return false;
    unsigned port = 0;
    for (size_t i = hostEnd + 1; i < authority; ++i)
    {
        if (host[i] < '0' || host[i] > '9') return false;
        port = port * 10 + (host[i] - '0');
    }
    return port > 0 && port <= 65535;
}

bool Settings::load()
{
    Nvs cfg(PREFS_NS, NVS_READWRITE);
    if (cfg.status != ESP_OK) return false;

    char password[64] = {};
    size_t len = sizeof(password);
    esp_err_t status = nvs_get_str(cfg.handle, "appass", password, &len);
    if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND) return false;
    factory = status == ESP_ERR_NVS_NOT_FOUND || strcmp(password, "admin123") == 0;
    if (factory)
    {
        len = sizeof(password);
        status = nvs_get_str(cfg.handle, "pairpass", password, &len);
        if (status == ESP_ERR_NVS_NOT_FOUND)
        {
            static constexpr char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
            static_assert(sizeof(alphabet) == 33, "pairing alphabet must have 32 characters");
            uint8_t random[16];
            esp_fill_random(random, sizeof(random));
            for (size_t i = 0; i < sizeof(random); ++i) password[i] = alphabet[random[i] & 31];
            password[sizeof(random)] = 0;
            status = nvs_set_str(cfg.handle, "pairpass", password);
            if (status == ESP_OK) status = nvs_commit(cfg.handle);
            mbedtls_platform_zeroize(random, sizeof(random));
        }
        if (status != ESP_OK || strlen(password) != 16) return false;
    }
    apPass = password;
    const size_t passLength = strlen(password);
    mbedtls_platform_zeroize(password, sizeof(password));
    if (apPass.length() != passLength || passLength < 8) return false;

    NetworkPrefs saved = {};
    len = sizeof(saved);
    status = nvs_get_blob(cfg.handle, "networkcfg", &saved, &len);
    if (status == ESP_OK && len == sizeof(saved) &&
        memchr(saved.network, 0, sizeof(saved.network)) && memchr(saved.rpc, 0, sizeof(saved.rpc)))
    {
        network = saved.network;
        customRpc = saved.rpc;
        autoLockMin = saved.minutes;
    }
    else if (status == ESP_ERR_NVS_NOT_FOUND)
    {
        Preferences old;
        if (!old.begin(PREFS_NS, true)) return false;
        network = old.getString("net", "devnet");
        customRpc = old.getString("rpc", "");
        autoLockMin = old.getUChar("lock", SOLDER_DEFAULT_AUTOLOCK_MIN);
        old.end();
    }
    else return false;

    if (!validNetwork(network, customRpc)) network = "devnet";
    if (network != "custom") customRpc = "";
    if (!validLockTime(autoLockMin)) autoLockMin = SOLDER_DEFAULT_AUTOLOCK_MIN;
    return true;
}

bool Settings::setApPassword(const String &password)
{
    if (password.length() < 8 || password.length() > 63 || password == "admin123" || password == apPass ||
        strlen(password.c_str()) != password.length()) return false;
    Nvs cfg(PREFS_NS, NVS_READWRITE);
    if (cfg.status != ESP_OK || nvs_set_str(cfg.handle, "appass", password.c_str()) != ESP_OK ||
        nvs_commit(cfg.handle) != ESP_OK) return false;
    clearString(apPass);
    apPass = password;
    factory = false;
    return true;
}

bool Settings::setNetwork(const String &net, const String &rpc, uint8_t minutes)
{
    if (!validLockTime(minutes) || !validNetwork(net, rpc)) return false;
    NetworkPrefs saved = {};
    net.toCharArray(saved.network, sizeof(saved.network));
    if (net == "custom") rpc.toCharArray(saved.rpc, sizeof(saved.rpc));
    saved.minutes = minutes;
    Nvs cfg(PREFS_NS, NVS_READWRITE);
    if (cfg.status != ESP_OK || nvs_set_blob(cfg.handle, "networkcfg", &saved, sizeof(saved)) != ESP_OK ||
        nvs_commit(cfg.handle) != ESP_OK) return false;
    network = net;
    customRpc = saved.rpc;
    autoLockMin = minutes;
    return true;
}

bool WalletSlot::exists()
{
    Nvs nvs(WALLET_NS, NVS_READONLY);
    if (nvs.status != ESP_OK) return nvs.status != ESP_ERR_NVS_NOT_FOUND;
    size_t len = 0;
    return nvs_get_blob(nvs.handle, WALLET_KEY, nullptr, &len) != ESP_ERR_NVS_NOT_FOUND;
}

String Settings::rpcUrl() const
{
    if (network == "custom" && customRpc.length()) return customRpc;
    return network == "mainnet" ? SOLDER_MAINNET_RPC : SOLDER_DEVNET_RPC;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace solder
{
    Settings settings;
    WalletSlot store;
    Request request;

    // Only used for offline helpers (signing, ATA derivation). It never connects.
    static Infratic offline("http://offline");

    static Keypair wallet;          // only filled while unlocked
    static bool isUnlocked = false;
    static String sessionToken;
    static unsigned long lastActivity = 0;
    static String generated;        // mnemonic shown during setup, not saved yet
    static uint32_t nextRequestId = 1;
    static uint32_t restartAt = 0;
    static bool restartPending = false;
    static uint32_t lastPinFailure = 0, pinDelay = 0;
    static uint32_t pressStart = 0;
    static bool holding = false, needRelease = false;
    static bool rawPressed = false, stablePressed = false;
    static uint32_t rawChangedAt = 0, lastButtonSample = 0;
    static unsigned page = 0, pages = 1;

    enum class PinGate { Idle, Waiting, Ready };
    static PinGate pinGate = PinGate::Idle;
    static uint32_t gateSince = 0;
    static String pendingPin, pendingPinClient;

    static void clearPinGate()
    {
        clearString(pendingPin);
        clearString(pendingPinClient);
        pinGate = PinGate::Idle;
    }

    static bool pinGateExpired(uint32_t now)
    {
        const uint32_t limit = pinGate == PinGate::Waiting ? SOLDER_REQUEST_TIMEOUT_MS : 30000;
        return pinGate != PinGate::Idle && now - gateSince >= limit;
    }

    static bool sameSecret(const String &a, const String &b)
    {
        if (a.length() != b.length()) return false;
        volatile uint8_t difference = 0;
        for (unsigned i = 0; i < a.length(); ++i) difference |= a[i] ^ b[i];
        return !difference;
    }

    static void resetButton(uint32_t now = millis())
    {
        holding = false;
        needRelease = true;
        rawPressed = digitalRead(SOLDER_BUTTON_PIN) == LOW;
        stablePressed = true; // arm only after observing a stable release
        rawChangedAt = lastButtonSample = now;
    }

    static bool sampleButton(uint32_t now)
    {
        // If work on the main loop prevented sampling, elapsed time cannot
        // attest to a continuous physical hold.
        if (now - lastButtonSample > 250) resetButton(now);
        lastButtonSample = now;
        const bool pressed = digitalRead(SOLDER_BUTTON_PIN) == LOW;
        if (pressed != rawPressed)
        {
            rawPressed = pressed;
            rawChangedAt = now;
        }
        if (now - rawChangedAt >= 35) stablePressed = rawPressed;
        return stablePressed;
    }

    static void clearRequest()
    {
        if (!request.message.empty()) mbedtls_platform_zeroize(request.message.data(), request.message.size());
        request = Request();
        resetButton();
    }

    static void showIdle()
    {
        display::idle(isUnlocked ? wallet.publicBase58() : String(""), !isUnlocked);
    }

    void restartSoon(uint32_t ms)
    {
        restartAt = millis() + ms;
        restartPending = true;
    }

    // ------------------------------------------------------------- session

    bool unlocked() { return isUnlocked; }

    String address() { return isUnlocked ? wallet.publicBase58() : String(""); }

    static String randomHex(size_t n)
    {
        static const char *hex = "0123456789abcdef";
        String s;
        for (size_t i = 0; i < n; i++)
        {
            uint8_t b = esp_random() & 0xFF;
            s += hex[b >> 4];
            s += hex[b & 15];
        }
        return s;
    }

    // Reserve attempts durably before doing expensive PIN derivation. Include the
    // old firmware's consecutive failures during migration.
    static bool readAttempts(uint8_t &count)
    {
        Nvs slot(WALLET_NS, NVS_READONLY);
        if (slot.status != ESP_OK) return false;
        // Solder writes version-1 BIP39 slots with the library's 20,000-round
        // KDF. Reject corrupt metadata before it controls allocations or KDF work.
        uint8_t blob[50 + 32];
        size_t len = sizeof(blob);
        static constexpr uint8_t header[] = {1, 1, 0x20, 0x4e, 0, 0};
        if (nvs_get_blob(slot.handle, WALLET_KEY, blob, &len) != ESP_OK ||
            len < 50 + 16 || (len - 50) % 4 != 0 || memcmp(blob, header, sizeof(header)) != 0) return false;
        count = 0;
        for (const char *key : {"wallet_f", "reserved"})
        {
            uint8_t n = 0;
            esp_err_t err = nvs_get_u8(slot.handle, key, &n);
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
            if (n > count) count = n;
        }
        return true;
    }

    static bool recordAttempts(uint8_t count)
    {
        Nvs slot(WALLET_NS, NVS_READWRITE);
        return slot.status == ESP_OK && nvs_set_u8(slot.handle, "reserved", count) == ESP_OK &&
               nvs_commit(slot.handle) == ESP_OK;
    }

    static bool eraseWallet()
    {
        Nvs slot(WALLET_NS, NVS_READWRITE);
        if (slot.status != ESP_OK || nvs_erase_key(slot.handle, WALLET_KEY) != ESP_OK ||
            nvs_commit(slot.handle) != ESP_OK) return false;
        // A new setup clears the counters again before creating the next wallet.
        return !store.exists();
    }

    static bool allowPinAttempt(uint8_t count, const String &pin, const String &client, String &error)
    {
        if (count < 5) return true;
        if (client.length() != 32 || strspn(client.c_str(), "0123456789abcdef") != 32)
        {
            error = "Reopen the wallet page before confirming a PIN attempt";
            return false;
        }
        const uint32_t now = millis();
        if (pinGateExpired(now)) clearPinGate();
        if (request.state == ReqState::Waiting)
        {
            error = "Finish the pending request before unlocking";
            return false;
        }
        if (pinGate == PinGate::Idle)
        {
            pendingPinClient = client;
            pendingPin = pin;
            if (pendingPinClient.length() != 32 || !pin.length() || pendingPin.length() != pin.length())
            {
                clearPinGate();
                error = "Could not stage PIN confirmation";
                return false;
            }
            pinGate = PinGate::Waiting;
            gateSince = now;
            resetButton();
            display::text("Allow one PIN try?", "Hold & release BOOT");
        }
        else if (!sameSecret(client, pendingPinClient))
        {
            error = "Another PIN confirmation is pending. Wait for it to expire.";
            return false;
        }
        else if (!sameSecret(pin, pendingPin))
        {
            error = "A different PIN is awaiting confirmation. Retry the original PIN or wait for it to expire.";
            return false;
        }
        else if (pinGate == PinGate::Ready)
        {
            clearPinGate();
            return true;
        }
        error = "Hold and release BOOT, then try the PIN again";
        return false;
    }

    bool unlock(const String &pin, String &outToken, String &outError, const String &pinClient)
    {
        if (!store.exists())
        {
            outError = "No wallet to unlock";
            return false;
        }
        if (uint32_t(millis() - lastPinFailure) < pinDelay)
        {
            outError = "Wait a few seconds before the next try";
            return false;
        }
        uint8_t prior = 0;
        if (!readAttempts(prior))
        {
            outError = "Could not read PIN attempts";
            return false;
        }
        if (!allowPinAttempt(prior, pin, pinClient, outError)) return false;
        if (prior >= SOLDER_MAX_PIN_ATTEMPTS)
        {
            // A power loss may have interrupted erasure after the last reservation.
            lock();
            outError = eraseWallet() ? "PIN attempts exhausted. Restore your backup words." : "Wallet erase failed";
            return false;
        }
        const uint8_t attempt = prior + 1;
        if (!recordAttempts(attempt))
        {
            outError = "Could not record PIN attempt";
            return false;
        }
        Keypair kp;
        if (!store.unlock(pin, kp))
        {
            if (attempt >= SOLDER_MAX_PIN_ATTEMPTS)
            {
                lock();
                outError = eraseWallet() ? "Too many wrong PINs, the wallet was erased. Restore it with your backup words."
                                         : "Wallet erase failed";
            }
            else
            {
                const int left = SOLDER_MAX_PIN_ATTEMPTS - attempt;
                outError = "Wrong PIN, " + String(left) + (left == 1 ? " try left" : " tries left");
                lastPinFailure = millis();
                pinDelay = 1000UL * attempt;
            }
            return false;
        }
        lock();
        wallet = std::move(kp);
        sessionToken = randomHex(16);
        outToken = sessionToken;
        if (!recordAttempts(0) || wallet.privkey.size() != 64 || outToken.length() != 32)
        {
            lock();
            clearString(outToken);
            outError = "Could not open a session";
            return false;
        }
        pinDelay = 0;
        isUnlocked = true;
        lastActivity = millis();
        showIdle();
        return true;
    }

    void lock()
    {
        if (!wallet.privkey.empty()) mbedtls_platform_zeroize(wallet.privkey.data(), wallet.privkey.size());
        wallet = Keypair();
        isUnlocked = false;
        clearString(sessionToken);
        clearRequest();
        clearPinGate();
        showIdle();
    }

    static bool expireSession()
    {
        if (isUnlocked && settings.autoLockMin &&
            uint32_t(millis() - lastActivity) >= settings.autoLockMin * 60000UL) lock();
        return !isUnlocked;
    }

    bool checkSession(const String &token, bool touch)
    {
        if (expireSession() || token.length() != 32 || !sameSecret(token, sessionToken)) return false;
        if (touch) lastActivity = millis();
        return true;
    }

    // ------------------------------------------------------------- setup

    const String &pendingMnemonic() { return generated; }

    void clearPendingMnemonic() { clearString(generated); }

    const String &generateMnemonic(uint8_t words)
    {
        clearPendingMnemonic();
        generated = Mnemonic::generate(words == 24 ? 24 : 12);
        if (!generated.length() || std::count(generated.begin(), generated.end(), ' ') != words - 1 ||
            !Mnemonic::isValid(generated)) clearPendingMnemonic();
        return generated;
    }

    bool finishSetup(const String &mnemonic, const String &pin, String &outError)
    {
        if (store.exists())
        {
            outError = "A wallet already exists";
            return false;
        }
        if (pin.length() != 6 || strspn(pin.c_str(), "0123456789") != 6)
        {
            outError = "Use a six-digit PIN";
            return false;
        }
        // The library accepts whitespace-separated English words case-insensitively.
        if (!recordAttempts(0) || !store.saveMnemonic(mnemonic, pin))
        {
            outError = "Could not save the wallet. Check the backup words and their order.";
            return false;
        }
        clearPendingMnemonic();
        pinDelay = 0;
        clearPinGate();
        return true;
    }

    // ------------------------------------------------------------- requests

    bool startRequest(Request &r, String &outError)
    {
        if (expireSession())
        {
            outError = "Unlock the wallet first";
            return false;
        }
        if (request.state == ReqState::Waiting)
        {
            outError = "Another request is waiting for the button";
            return false;
        }
        if (!display::available() || !(pages = display::reviewPages(r)))
        {
            outError = "Device display is unavailable";
            return false;
        }
        clearRequest();
        clearPinGate();
        page = 0;
        r.id = nextRequestId++;
        r.state = ReqState::Waiting;
        r.createdMs = millis();
        r.hold = 0;
        r.signature = r.tx = r.error = "";
        request = std::move(r);
        display::request(request, page);
        return true;
    }

    void rejectRequest()
    {
        if (request.state == ReqState::Waiting) request.state = ReqState::Rejected;
        showIdle();
    }

    // Runs right after the button was held. Signing takes a few milliseconds.
    static void execute()
    {
        Request &r = request;

        if (r.kind == ReqKind::Wipe)
        {
            if (!eraseWallet())
            {
                r.state = ReqState::Failed;
                r.error = "Could not erase the wallet";
                return;
            }
            const uint32_t id = r.id;
            lock();
            r.id = id;
            r.kind = ReqKind::Wipe;
            r.state = ReqState::Done;
            r.createdMs = millis();
            return;
        }

        if (r.kind == ReqKind::SignMessage)
        {
            uint8_t sig[64];
            if (!offline.signMessageRaw(r.message, wallet.privkey, sig))
            {
                r.error = "Signing failed";
                r.state = ReqState::Failed;
                return;
            }
            r.signature = base58Encode(sig, 64);
            r.state = ReqState::Done;
            return;
        }

        // Solder builds the transaction itself from what it showed you, the browser
        // only supplies the recent blockhash.
        Transaction tx;
        tx.fee_payer = wallet.pubkey();
        tx.recent_blockhash = r.blockhash;
        Pubkey to = Pubkey::fromBase58(r.to);
        if (r.kind == ReqKind::SendSol)
        {
            std::vector<uint8_t> data = {2, 0, 0, 0};
            for (int i = 0; i < 8; i++) data.push_back((uint8_t)(r.amount >> (8 * i)));
            tx.add(Instruction(Pubkey::fromBase58(INFRATIC_SYSTEM_PROGRAM_ID),
                               {AccountMeta::writable(wallet.pubkey(), true), AccountMeta::writable(to)}, data));
        }
        else
        {
            String src, dst;
            if (!offline.getAssociatedTokenAddress(wallet.publicBase58(), r.mint, src, r.tokenProgram) ||
                !offline.getAssociatedTokenAddress(r.to, r.mint, dst, r.tokenProgram))
            {
                r.error = "Could not derive token accounts";
                r.state = ReqState::Failed;
                return;
            }
            Pubkey mint = Pubkey::fromBase58(r.mint), prog = Pubkey::fromBase58(r.tokenProgram);
            Pubkey dstAta = Pubkey::fromBase58(dst);
            tx.add(Infratic::createAssociatedTokenAccountIdempotentIx(wallet.pubkey(), dstAta, to, mint, prog));
            tx.add(Infratic::transferCheckedIx(Pubkey::fromBase58(src), mint, dstAta, wallet.pubkey(),
                                               r.amount, r.decimals, prog));
        }

        // Move the live key into the list required by the signing API. An
        // initializer list would leave unwiped copies in freed heap blocks.
        std::vector<Keypair> signers;
        signers.push_back(std::move(wallet));
        const bool signedOk = tx.sign(signers);
        wallet = std::move(signers.front());
        if (!signedOk)
        {
            r.error = "Could not sign this transaction";
            r.state = ReqState::Failed;
            return;
        }
        r.signature = base58Encode(tx.signature.data(), tx.signature.size());
        r.tx = tx.serializeBase64();
        r.state = ReqState::Done;
    }

    // ------------------------------------------------------------- main

    bool begin()
    {
        pinMode(SOLDER_BUTTON_PIN, INPUT_PULLUP);
        pinMode(SOLDER_LED_PIN, OUTPUT);
        digitalWrite(SOLDER_LED_PIN, LOW);
        // Erasure follows the app's durable attempt reservation and BOOT gate.
        store.setMaxAttempts(0);
        return settings.load();
    }

    static void checkPinButton(bool pressed, uint32_t now)
    {
        if (pinGate == PinGate::Idle) return;
        if (pinGateExpired(now))
        {
            clearPinGate();
            showIdle();
            return;
        }
        if (pinGate != PinGate::Waiting) return;
        if (needRelease) { if (!pressed) needRelease = false; return; }
        if (pressed && !holding)
        {
            holding = true;
            pressStart = now;
        }
        if (!pressed && holding)
        {
            holding = false;
            if (now - pressStart >= SOLDER_HOLD_MS)
            {
                pinGate = PinGate::Ready;
                gateSince = now;
                display::text("One PIN try allowed", "Enter PIN on phone");
            }
        }
        digitalWrite(SOLDER_LED_PIN, ((now / 250) & 1) ? HIGH : LOW);
    }

    static void review(bool pressed, uint32_t now)
    {
        if (now - request.createdMs >= SOLDER_REQUEST_TIMEOUT_MS)
        {
            request.state = ReqState::Expired;
            showIdle();
            return;
        }
        if (needRelease) { if (!pressed) needRelease = false; return; }
        if (pressed && !holding)
        {
            holding = true;
            pressStart = now;
        }
        if (!holding) return;

        const bool last = page + 1 == pages;
        const uint32_t heldMs = now - pressStart;
        if (!pressed)
        {
            holding = false;
            request.hold = 0;
            if (!last && heldMs >= 35)
            {
                if (!display::available())
                {
                    request.state = ReqState::Failed;
                    request.error = "Device display disconnected";
                    return;
                }
                ++page;
                request.createdMs = now;
                lastActivity = now;
            }
            display::request(request, page);
            return;
        }
        if (!last) return;
        request.hold = std::min(1.0f, heldMs / float(SOLDER_HOLD_MS));
        static uint32_t lastDraw = 0;
        if (now - lastDraw >= 80)
        {
            display::request(request, page);
            lastDraw = now;
        }
        if (heldMs >= SOLDER_HOLD_MS && rawPressed)
        {
            holding = false;
            if (!display::available())
            {
                request.state = ReqState::Failed;
                request.error = "Device display disconnected";
                return;
            }
            execute();
            showIdle();
        }
    }

    void loop()
    {
        uint32_t now = millis();
        if (restartPending && int32_t(now - restartAt) >= 0) ESP.restart();
        expireSession();
        bool pressed = sampleButton(now);
        if (request.state == ReqState::Waiting)
        {
            digitalWrite(SOLDER_LED_PIN, ((now / (pressed ? 80 : 400)) & 1) ? HIGH : LOW);
            review(pressed, now);
        }
        else
        {
            digitalWrite(SOLDER_LED_PIN, LOW);
            checkPinButton(pressed, now);
        }
    }
}
