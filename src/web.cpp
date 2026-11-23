#include "web.h"
#include "solder.h"
#include "display.h"
#include "web_ui.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <base58.h>

static WebServer server(80);
static DNSServer dns;
static String apName;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void sendJson(int code, const JsonDocument &doc)
{
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", out);
}

static void fail(int code, const String &message)
{
    StaticJsonDocument<384> d;
    d["error"] = message;
    sendJson(code, d);
}

static bool readBody(JsonDocument &doc)
{
    // JSON only: a form post from another website can't send this content type
    // without a CORS preflight, and Solder never answers preflights.
    if (!server.header("Content-Type").startsWith("application/json"))
    {
        fail(415, "Send JSON");
        return false;
    }
    if (deserializeJson(doc, server.arg("plain")))
    {
        fail(400, "Bad request");
        return false;
    }
    return true;
}

// Only answer to the names Solder really has. Stops DNS rebinding, where a
// website points its own domain at 192.168.4.1 to talk to the API as same origin.
static bool hostAllowed()
{
    String host = server.hostHeader();
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    host.toLowerCase();
    return host == WiFi.softAPIP().toString() || host == SOLDER_HOSTNAME ".local";
}

// Every wallet action needs the session from /api/unlock. The custom header also
// keeps other websites from calling the API (no CORS, so they can't set it).
static bool session()
{
    if (solder::checkSession(server.header("X-Solder-Session"))) return true;
    fail(401, solder::unlocked() ? "Session ended, unlock again" : "Wallet is locked");
    return false;
}

// Settings need the session once a wallet exists
static bool ownerOrNew()
{
    return !solder::store.exists() || session();
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
static bool parseAmount(const String &text, uint8_t decimals, uint64_t &out)
{
    String t = text;
    t.trim();
    if (!t.length()) return false;
    uint64_t whole = 0, frac = 0;
    int fracDigits = 0;
    bool dot = false;
    for (unsigned i = 0; i < t.length(); i++)
    {
        char c = t[i];
        if (c == '.' || c == ',')
        {
            if (dot) return false;
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (dot)
        {
            if (++fracDigits > decimals) return false;
            frac = frac * 10 + (c - '0');
        }
        else
        {
            if (whole > 1844674407ULL) return false;
            whole = whole * 10 + (c - '0');
        }
    }
    uint64_t scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    for (int i = fracDigits; i < decimals; i++) frac *= 10;
    if (whole > (UINT64_MAX - frac) / scale) return false;
    out = whole * scale + frac;
    return out > 0;
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
// pages
// ---------------------------------------------------------------------------

static void handleIndex()
{
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html", (const char *)WEB_UI_GZ, WEB_UI_GZ_LEN);
}

// Phones probe these URLs after joining a WiFi; redirecting opens the wallet
static void handleCaptive()
{
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
}

static void handleNotFound()
{
    if (server.uri().startsWith("/api/"))
    {
        fail(404, "Unknown endpoint");
        return;
    }
    handleCaptive();
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
    d["unlocked"] = solder::unlocked();
    d["address"] = solder::address();
    d["network"] = solder::settings.network;
    d["rpc"] = solder::settings.rpcUrl();
    d["autoLockMin"] = solder::settings.autoLockMin;
    d["screen"] = (bool)SOLDER_OLED;
    d["holdMs"] = SOLDER_HOLD_MS;
    d["timeoutS"] = SOLDER_REQUEST_TIMEOUT_MS / 1000;
    sendJson(200, d);
}

static void handleApPassword()
{
    // the factory password can be replaced by whoever is on the WiFi (they know it),
    // after that only the wallet owner can change it
    if (!solder::settings.defaultPassword() && !ownerOrNew()) return;
    StaticJsonDocument<192> in;
    if (!readBody(in)) return;
    String pass = in["password"] | "";
    if (pass.length() < 8 || pass.length() > 63)
    {
        fail(400, "Use 8 to 63 characters");
        return;
    }
    if (pass == SOLDER_DEFAULT_AP_PASS)
    {
        fail(400, "Choose a password other than the factory one");
        return;
    }
    solder::settings.apPass = pass;
    solder::settings.save();
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
    StaticJsonDocument<64> in;
    if (!readBody(in)) return;
    String words = solder::generateMnemonic(in["words"] | 12);
    DynamicJsonDocument d(512);
#if SOLDER_OLED
    // The words never leave the device, they are shown on its screen
    display::words(words, 0);
    d["onScreen"] = true;
    d["count"] = in["words"] == 24 ? 24 : 12;
#else
    d["words"] = words;
#endif
    sendJson(200, d);
}

static void handleWordsPage()
{
    StaticJsonDocument<64> in;
    if (!readBody(in)) return;
    if (solder::store.exists() || !solder::pendingMnemonic().length())
    {
        fail(409, "No backup words to show");
        return;
    }
    display::words(solder::pendingMnemonic(), in["page"] | 0);
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

static void handleFinish()
{
    if (!passwordChanged()) return;
    DynamicJsonDocument in(1024);
    if (!readBody(in)) return;
    String mnemonic = in["mnemonic"] | "";
    if (!mnemonic.length()) mnemonic = solder::pendingMnemonic();
    String error;
    if (!solder::finishSetup(mnemonic, in["pin"] | "", error))
    {
        fail(400, error);
        return;
    }
    String token;
    solder::unlock(in["pin"] | "", token, error);
    StaticJsonDocument<192> d;
    d["token"] = token;
    d["address"] = solder::address();
    sendJson(200, d);
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

static void handleUnlock()
{
    StaticJsonDocument<128> in;
    if (!readBody(in)) return;
    String token, error;
    if (!solder::unlock(in["pin"] | "", token, error))
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
    solder::lock();
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

// ---------------------------------------------------------------------------
// requests that need the button
// ---------------------------------------------------------------------------

static void handleSend()
{
    if (!session()) return;
    StaticJsonDocument<768> in;
    if (!readBody(in)) return;
    Request r;
    r.to = in["to"] | "";
    r.to.trim();
    r.amountText = in["amount"] | "";
    r.mint = in["mint"] | "";
    r.blockhash = in["blockhash"] | "";
    if (!decode32(r.to))
    {
        fail(400, "That is not a Solana address");
        return;
    }
    if (r.to == solder::address())
    {
        fail(400, "This is your own address");
        return;
    }
    if (!decode32(r.blockhash))
    {
        fail(400, "Missing recent blockhash, is your phone online?");
        return;
    }

    if (r.mint.length())
    {
        // decimals and token program come from the browser; TransferChecked makes the
        // chain reject the transaction if they don't match the mint
        String prog = in["program"] | INFRATIC_TOKEN_PROGRAM_ID;
        if (!decode32(r.mint) || (prog != INFRATIC_TOKEN_PROGRAM_ID && prog != INFRATIC_TOKEN_2022_PROGRAM_ID))
        {
            fail(400, "Unknown token");
            return;
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
    if (r.decimals > 18 || !parseAmount(r.amountText, r.decimals, r.amount))
    {
        fail(400, "Enter an amount with at most " + String(r.decimals) + " decimals");
        return;
    }
    r.amountText.replace(",", ".");
    String error;
    if (!solder::startRequest(r, error))
    {
        fail(409, error);
        return;
    }
    respondRequest();
}

static void handleSign()
{
    if (!session()) return;
    DynamicJsonDocument in(2048);
    if (!readBody(in)) return;
    String msg = in["message"] | "";
    if (!msg.length() || msg.length() > 1024)
    {
        fail(400, "Write a message up to 1024 characters");
        return;
    }
    Request r;
    r.kind = ReqKind::SignMessage;
    r.message.assign((const uint8_t *)msg.c_str(), (const uint8_t *)msg.c_str() + msg.length());
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
    // after an erase there is no wallet and no session, the result is still readable
    if (solder::store.exists() && !session()) return;
    if (solder::request.state == ReqState::None)
    {
        fail(404, "No request");
        return;
    }
    respondRequest();
}

static void handleReject()
{
    if (solder::store.exists() && !session()) return;
    solder::rejectRequest();
    respondRequest();
}

static void handleNetwork()
{
    if (!ownerOrNew()) return;
    StaticJsonDocument<384> in;
    if (!readBody(in)) return;
    String net = in["network"] | "devnet";
    String rpc = in["rpc"] | "";
    if (net != "devnet" && net != "mainnet" && net != "custom")
    {
        fail(400, "Unknown network");
        return;
    }
    if (net == "custom" && !rpc.startsWith("https://") && !rpc.startsWith("http://"))
    {
        fail(400, "The RPC address must start with https://");
        return;
    }
    solder::settings.network = net;
    solder::settings.customRpc = rpc;
    if (in.containsKey("autoLockMin")) solder::settings.autoLockMin = in["autoLockMin"];
    solder::settings.save();
    StaticJsonDocument<32> d;
    d["ok"] = true;
    sendJson(200, d);
}

// ---------------------------------------------------------------------------

static void api(const char *path, HTTPMethod method, void (*handler)())
{
    server.on(path, method, [handler]() {
        if (!hostAllowed())
        {
            fail(403, "Open Solder at http://" + WiFi.softAPIP().toString());
            return;
        }
        handler();
    });
}

namespace web
{
    void begin(const String &ap)
    {
        apName = ap;
        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(53, "*", WiFi.softAPIP());

        server.on("/", HTTP_GET, handleIndex);
        for (const char *p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/ncsi.txt", "/connecttest.txt"})
            server.on(p, HTTP_GET, handleCaptive);

        api("/api/state", HTTP_GET, handleState);
        api("/api/ap-password", HTTP_POST, handleApPassword);
        api("/api/setup/generate", HTTP_POST, handleGenerate);
        api("/api/setup/words", HTTP_POST, handleWordsPage);
        api("/api/setup/finish", HTTP_POST, handleFinish);
        api("/api/unlock", HTTP_POST, handleUnlock);
        api("/api/lock", HTTP_POST, handleLock);
        api("/api/send", HTTP_POST, handleSend);
        api("/api/sign", HTTP_POST, handleSign);
        api("/api/wipe", HTTP_POST, handleWipe);
        api("/api/request", HTTP_GET, handleRequest);
        api("/api/request/reject", HTTP_POST, handleReject);
        api("/api/network", HTTP_POST, handleNetwork);
        server.onNotFound(handleNotFound);

        const char *headers[] = {"X-Solder-Session", "Content-Type"};
        server.collectHeaders(headers, 2);
        server.begin();
    }

    void loop()
    {
        dns.processNextRequest();
        server.handleClient();
    }
}
