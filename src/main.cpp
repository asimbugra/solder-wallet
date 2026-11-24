// Solder: a Solana hardware wallet on an ESP32.
//
// The keys are created on the device and kept encrypted with your PIN.
// Solder never joins a network: it runs its own WiFi, your phone connects to it,
// asks for a signature, and Solder signs only after you hold the BOOT button.
// The phone fetches balances and broadcasts what Solder signed.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <bootloader_random.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <mbedtls/platform_util.h>
#include "solder.h"
#include "web.h"
#include "display.h"

static String apName;
static bool serving = false;

// Arduino's prebuilt ESP-IDF panic handler normally saves task stacks to flash.
// The linker redirects that call here, including when the flash cache is off.
extern "C" void IRAM_ATTR __wrap_esp_core_dump_write(panic_info_t *) {}

static bool clearOldCrashDump()
{
    const esp_partition_t *dump = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!dump) return true;
    // Check the entire diagnostic partition, including incomplete old dumps.
    uint8_t block[256];
    for (size_t offset = 0; offset < dump->size; offset += sizeof(block))
    {
        size_t size = std::min(sizeof(block), size_t(dump->size - offset));
        const esp_err_t read = esp_partition_read_raw(dump, offset, block, size);
        const bool blank = read == ESP_OK && std::all_of(block, block + size, [](uint8_t b) { return b == 0xff; });
        mbedtls_platform_zeroize(block, sizeof(block));
        if (read != ESP_OK) return false;
        if (!blank) return esp_partition_erase_range(dump, 0, dump->size) == ESP_OK;
    }
    return true;
}

static bool startAccessPoint()
{
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP)) return false;

    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    char name[24];
    snprintf(name, sizeof(name), "Solder-%02X%02X", mac[4], mac[5]);
    apName = name;

    if (!WiFi.softAP(apName.c_str(), solder::settings.apPass.c_str())) return false;
    // Drop any stations associated while the driver was starting.
    if (esp_wifi_deauth_sta(0) != ESP_OK) return false;
    if (MDNS.begin(SOLDER_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    return true;
}

void setup()
{
    Serial.begin(115200);
    if (!clearOldCrashDump())
    {
        Serial.println("Could not clear previous crash data. Check flash storage.");
        return;
    }
    display::begin();
    if (!display::available())
    {
        Serial.println("OLED unavailable. Check the display and restart.");
        return;
    }
    // The factory pairing password needs hardware entropy before WiFi starts.
    bootloader_random_enable();
    bool initialized = solder::begin();
    bootloader_random_disable();
    if (!initialized || !startAccessPoint())
    {
        WiFi.mode(WIFI_OFF);
        Serial.println("Wallet initialization failed. Check storage and WiFi.");
        display::text("Startup failed", "Check USB console");
        return;
    }
    if (!web::begin(apName))
    {
        WiFi.mode(WIFI_OFF);
        Serial.println("Wallet server initialization failed.");
        return;
    }
    serving = true;

    bool factory = solder::settings.defaultPassword();
    Serial.println();
    Serial.println("Solder " SOLDER_VERSION);
    Serial.println("WiFi:     " + apName);
    Serial.println(factory ? "Password: " + solder::settings.apPass + "  (factory, change on first connection)"
                           : "Password: the one you set");
    Serial.println("Open      http://192.168.4.1");
    display::boot(apName, factory ? solder::settings.apPass : String("(your password)"));
}

void loop()
{
    if (serving)
    {
        solder::loop();
        web::loop();
    }
    delay(2);
}
