#include "solder.h"
#include "display.h"
#include <Preferences.h>
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

static const char *PREFS_NS = "solder_cfg";

void Settings::load()
{
    Preferences p;
    p.begin(PREFS_NS, true);
    apPass = p.getString("appass", SOLDER_DEFAULT_AP_PASS);
    network = p.getString("net", "devnet");
    customRpc = p.getString("rpc", "");
    autoLockMin = p.getUChar("lock", SOLDER_DEFAULT_AUTOLOCK_MIN);
    p.end();
    if (apPass.length() < 8) apPass = SOLDER_DEFAULT_AP_PASS;
}

void Settings::save()
{
    Preferences p;
    p.begin(PREFS_NS, false);
    p.putString("appass", apPass);
    p.putString("net", network);
    p.putString("rpc", customRpc);
    p.putUChar("lock", autoLockMin);
    p.end();
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
    KeyStore store("wallet");
    Request request;

    // Only used for offline helpers (signing, ATA derivation). It never connects.
    static Infratic offline("http://offline");

    static Keypair wallet;          // only filled while unlocked
    static bool isUnlocked = false;
    static String sessionToken;
    static unsigned long lastActivity = 0;
    static String generated;        // mnemonic shown during setup, not saved yet
    static uint32_t nextRequestId = 1;
    static unsigned long pressStart = 0;
    static uint32_t pinDelayUntil = 0;
    static unsigned long restartAt = 0;

    static void wipeKey()
    {
        for (auto &b : wallet.privkey) b = 0;
        wallet = Keypair();
    }

    static void showIdle()
    {
        display::idle(isUnlocked ? wallet.publicBase58() : String(""), !isUnlocked);
    }

    String shortAddress(const String &a)
    {
        if (a.length() < 12) return a;
        return a.substring(0, 5) + ".." + a.substring(a.length() - 5);
    }

    void restartSoon(uint32_t ms) { restartAt = millis() + ms; }

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

    bool unlock(const String &pin, String &outToken, String &outError)
    {
        if (millis() < pinDelayUntil)
        {
            outError = "Wait a few seconds before the next try";
            return false;
        }
        Keypair kp;
        if (!store.unlock(pin, kp))
        {
            uint8_t failed = store.failedAttempts();
            if (!store.exists())
            {
                outError = "Too many wrong PINs, the wallet was erased. Restore it with your backup words.";
            }
            else
            {
                int left = SOLDER_MAX_PIN_ATTEMPTS - failed;
                outError = "Wrong PIN, " + String(left) + (left == 1 ? " try left" : " tries left");
                pinDelayUntil = millis() + 1000UL * std::min<int>(failed, 30); // slows down guessing
            }
            return false;
        }
        wipeKey();
        wallet = kp;
        for (auto &b : kp.privkey) b = 0;
        isUnlocked = true;
        sessionToken = randomHex(16);
        lastActivity = millis();
        outToken = sessionToken;
        showIdle();
        return true;
    }

    void lock()
    {
        wipeKey();
        isUnlocked = false;
        sessionToken = "";
        if (request.state == ReqState::Waiting) request.state = ReqState::Rejected;
        showIdle();
    }

    bool checkSession(const String &token)
    {
        if (!isUnlocked || token.length() != 32 || token != sessionToken) return false;
        lastActivity = millis();
        return true;
    }

    // ------------------------------------------------------------- setup

    String pendingMnemonic() { return generated; }

    String generateMnemonic(uint8_t words)
    {
        generated = Mnemonic::generate(words == 24 ? 24 : 12);
        return generated;
    }

    bool finishSetup(const String &mnemonic, const String &pin, String &outError)
    {
        if (store.exists())
        {
            outError = "A wallet already exists";
            return false;
        }
        if (pin.length() < 6)
        {
            outError = "Use a PIN with at least 6 digits";
            return false;
        }
        String m = mnemonic;
        m.trim();
        m.toLowerCase();
        if (!Mnemonic::isValid(m))
        {
            outError = "These words are not a valid backup phrase. Check the spelling and the order.";
            return false;
        }
        if (!store.saveMnemonic(m, pin))
        {
            outError = "Could not save the wallet";
            return false;
        }
        generated = "";
        return true;
    }

    // ------------------------------------------------------------- requests

    bool startRequest(Request &r, String &outError)
    {
        if (!isUnlocked)
        {
            outError = "Unlock the wallet first";
            return false;
        }
        if (request.state == ReqState::Waiting)
        {
            outError = "Another request is waiting for the button";
            return false;
        }
        r.id = nextRequestId++;
        r.state = ReqState::Waiting;
        r.createdMs = millis();
        r.hold = 0;
        r.signature = r.tx = r.error = "";
        request = r;
        pressStart = 0;
        display::request(request);
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
            store.wipe();
            lock();
            r.state = ReqState::Done;
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
            offline.getAssociatedTokenAddress(wallet.publicBase58(), r.mint, src, r.tokenProgram);
            offline.getAssociatedTokenAddress(r.to, r.mint, dst, r.tokenProgram);
            Pubkey mint = Pubkey::fromBase58(r.mint), prog = Pubkey::fromBase58(r.tokenProgram);
            Pubkey dstAta = Pubkey::fromBase58(dst);
            tx.add(Infratic::createAssociatedTokenAccountIdempotentIx(wallet.pubkey(), dstAta, to, mint, prog));
            tx.add(Infratic::transferCheckedIx(Pubkey::fromBase58(src), mint, dstAta, wallet.pubkey(),
                                               r.amount, r.decimals, prog));
        }

        if (!tx.sign({wallet}))
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

    void begin()
    {
        pinMode(SOLDER_BUTTON_PIN, INPUT_PULLUP);
        pinMode(SOLDER_LED_PIN, OUTPUT);
        digitalWrite(SOLDER_LED_PIN, LOW);
        store.setMaxAttempts(SOLDER_MAX_PIN_ATTEMPTS);
        settings.load();
    }

    void loop()
    {
        unsigned long now = millis();

        if (restartAt && (long)(now - restartAt) >= 0)
        {
            ESP.restart();
        }

        bool pressed = digitalRead(SOLDER_BUTTON_PIN) == LOW;
        if (request.state == ReqState::Waiting)
        {
            if (now - request.createdMs > SOLDER_REQUEST_TIMEOUT_MS)
            {
                request.state = ReqState::Expired;
                showIdle();
            }
            else if (pressed)
            {
                if (!pressStart) pressStart = now;
                float h = (now - pressStart) / (float)SOLDER_HOLD_MS;
                request.hold = h > 1 ? 1 : h;
                static unsigned long lastDraw = 0;
                if (now - lastDraw > 80)
                {
                    display::request(request);
                    lastDraw = now;
                }
                if (h >= 1)
                {
                    pressStart = 0;
                    digitalWrite(SOLDER_LED_PIN, HIGH);
                    execute();
                    showIdle();
                }
            }
            else if (pressStart)
            {
                pressStart = 0; // released too early
                request.hold = 0;
                display::request(request);
            }
            // blink while waiting, faster while held
            if (request.state == ReqState::Waiting)
                digitalWrite(SOLDER_LED_PIN, ((now / (pressed ? 80 : 400)) & 1) ? HIGH : LOW);
        }
        else
        {
            digitalWrite(SOLDER_LED_PIN, LOW);
        }

        if (isUnlocked && settings.autoLockMin && now - lastActivity > settings.autoLockMin * 60000UL)
        {
            lock();
        }
    }
}
