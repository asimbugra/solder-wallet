#pragma once

// Pins (ESP32 DevKit). BOOT is GPIO0 on almost every board.
#ifndef SOLDER_BUTTON_PIN
#define SOLDER_BUTTON_PIN 0
#endif
#ifndef SOLDER_LED_PIN
#define SOLDER_LED_PIN 2
#endif
#define SOLDER_OLED_SDA 21
#define SOLDER_OLED_SCL 22

// Solder never joins another network, it only runs its own access point.
// WPA2 needs at least 8 characters, so the factory password is admin123.
#define SOLDER_DEFAULT_AP_PASS "admin123"

// Hold the button this long to approve a request
#define SOLDER_HOLD_MS 1500
// A request that is not approved in time is dropped. A Solana blockhash lives
// about a minute, so the browser has time left to broadcast what was signed.
#define SOLDER_REQUEST_TIMEOUT_MS 45000
// Lock after this much time without any request from the browser
#define SOLDER_DEFAULT_AUTOLOCK_MIN 5
// Wrong PINs before the wallet erases itself (the backup words still restore it)
#define SOLDER_MAX_PIN_ATTEMPTS 10

#define SOLDER_VERSION "0.2.0"
#define SOLDER_HOSTNAME "solder"
#define SOLDER_DEVNET_RPC "https://api.devnet.solana.com"
#define SOLDER_MAINNET_RPC "https://api.mainnet-beta.solana.com"

#ifndef SOLDER_OLED
#define SOLDER_OLED 0
#endif
