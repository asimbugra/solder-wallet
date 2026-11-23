// Solder: a Solana hardware wallet on an ESP32.
//
// The keys are created on the device and kept encrypted with your PIN.
// Solder never joins a network: it runs its own WiFi, your phone connects to it,
// asks for a signature, and Solder signs only after you hold the BOOT button.
// The phone fetches balances and broadcasts what Solder signed.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "solder.h"
#include "web.h"
#include "display.h"

static String apName;

static void startAccessPoint()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP); // access point only, no station: Solder can't reach the internet

    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    char name[24];
    snprintf(name, sizeof(name), "Solder-%02X%02X", mac[4], mac[5]);
    apName = name;

    WiFi.softAP(apName.c_str(), solder::settings.apPass.c_str());
    MDNS.begin(SOLDER_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
}

void setup()
{
    Serial.begin(115200);
    display::begin();
    solder::begin();
    startAccessPoint();
    web::begin(apName);

    bool factory = solder::settings.defaultPassword();
    Serial.println();
    Serial.println("Solder " SOLDER_VERSION);
    Serial.println("WiFi:     " + apName);
    Serial.println(factory ? "Password: " SOLDER_DEFAULT_AP_PASS "  (factory, you'll be asked to change it)"
                           : "Password: the one you set");
    Serial.println("Open      http://192.168.4.1");
    display::boot(apName, factory ? String(SOLDER_DEFAULT_AP_PASS) : String("(your password)"));
}

void loop()
{
    web::loop();
    solder::loop();
    delay(2);
}
