#pragma once
#include <Arduino.h>
#include <Infratic-lib.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Settings kept in NVS (not secret, the wallet itself lives in KeyStore)
// ---------------------------------------------------------------------------
struct Settings
{
    String apPass;     // password of Solder's own WiFi
    String network;    // "devnet" | "mainnet" | "custom"
    String customRpc;  // used by the browser, Solder itself never goes online
    uint8_t autoLockMin = SOLDER_DEFAULT_AUTOLOCK_MIN;

    void load();
    void save();
    String rpcUrl() const;
    bool defaultPassword() const { return apPass == SOLDER_DEFAULT_AP_PASS; }
};

// ---------------------------------------------------------------------------
// A request that needs the button
// ---------------------------------------------------------------------------
enum class ReqKind : uint8_t { SendSol, SendToken, SignMessage, Wipe };
enum class ReqState : uint8_t { None, Waiting, Done, Rejected, Failed, Expired };

struct Request
{
    uint32_t id = 0;
    ReqKind kind = ReqKind::SendSol;
    ReqState state = ReqState::None;
    uint32_t createdMs = 0;
    float hold = 0;             // 0..1 while the button is held

    String to;                  // recipient owner address
    String mint;                // token mint (SendToken)
    String tokenProgram;
    uint64_t amount = 0;        // base units
    uint8_t decimals = 9;
    String amountText;          // as the user typed it, for display
    String symbol;              // "SOL" or short mint
    String blockhash;           // given by the browser
    std::vector<uint8_t> message;

    String signature;           // base58
    String tx;                  // signed transaction, base64, for the browser to broadcast
    String error;
};

const char *reqKindName(ReqKind k);
const char *reqStateName(ReqState s);

namespace solder
{
    extern Settings settings;
    extern KeyStore store;
    extern Request request;

    void begin();
    void loop();                       // button, LED, timeouts, auto lock, restart

    // session
    bool unlocked();
    String address();
    bool unlock(const String &pin, String &outToken, String &outError);
    void lock();
    bool checkSession(const String &token); // also counts as activity

    // setup
    String pendingMnemonic();          // words generated but not saved yet
    String generateMnemonic(uint8_t words);
    bool finishSetup(const String &mnemonic, const String &pin, String &outError);

    // requests
    bool startRequest(Request &r, String &outError);
    void rejectRequest();

    // restart after a short delay (so the HTTP answer still goes out)
    void restartSoon(uint32_t ms = 1500);

    String shortAddress(const String &a);
}
