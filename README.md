# Solder

A Solana hardware wallet you build yourself from a $5 ESP32.

Solder makes your keys, keeps them encrypted with your PIN, and never goes online. It runs its own WiFi; your phone joins it, asks for a signature, and Solder signs only after you hold its BOOT button. The phone reads balances from Solana and broadcasts what Solder signed.

Built on [Infratic-lib](https://github.com/infraticxyz-lab/Infratic-lib), the Solana library for ESP32.

![Solder demo](docs/demo.jpg)

## What it does

- Runs only its own WiFi access point. No station mode, no internet, no RPC calls from the device
- A unique setup password appears on the USB serial console and on the optional OLED. On first connection you choose your own WiFi password and reconnect
- Creates a wallet on the device (12 or 24 backup words, same addresses as Phantom and Solflare) or restores one
- Keeps the secret encrypted in flash with AES-256-GCM, key derived from your PIN; erases itself after 10 wrong PINs
- Builds and signs SOL and SPL / Token-2022 transfers itself, signs messages
- Every signature needs a fresh 1.5 s BOOT hold. On OLED, tap through the review pages first. A request expires after 45 s without physical review activity
- Optional OLED that shows what is being signed, independent of the phone

## How the pieces talk

```
 phone browser ──WiFi──> Solder (ESP32)            phone browser ──internet──> Solana RPC
   asks: send 0.25 SOL      shows it, waits for BOOT     reads balance, blockhash
   + recent blockhash       builds + signs the tx        broadcasts the signed tx
```

Your phone needs mobile data (or a second network) for balances and sending. Creating a wallet, signing messages and all key handling work with no internet at all.

If transfer confirmation is interrupted, reopen Solder in the same browser and unlock the wallet on the original network. The browser keeps the public signature of each pending transfer and checks its status again. **Check again** looks up the same signature. **Done** returns to the wallet, where pending transfers remain available until confirmed, rejected or removed from tracking.

## Hardware

| Part | Notes |
| --- | --- |
| ESP32 DevKit (ESP32-WROOM-32) | Any board with the BOOT button on GPIO0 |
| USB cable | For flashing and power |
| SSD1306 0.96" OLED, I2C (optional) | SDA 21, SCL 22, 3V3, GND |

## Build and flash

Install [PlatformIO](https://platformio.org/install), then:

```bash
pio run -e esp32dev -t upload          # board only
pio run -e esp32dev-oled -t upload     # with the OLED
pio device monitor
```

The web page in `web/index.html` is packed into the firmware on every build (`web/build.py`).

## First start

1. Read the 16-character setup password from the OLED or USB serial monitor at 115200 baud. If the startup message has already passed, reset the board with the monitor open. Join `Solder-XXXX` with this password. The page opens by itself, or go to `http://192.168.4.1`.
2. Choose a new WiFi password. Solder restarts; join again with the new password and the page continues.
3. Create a wallet and write the numbered words on paper. With an OLED, Solder asks for three words by position before you choose a 6-digit PIN. Find each word on your paper, tap BOOT to highlight it, then hold BOOT for 1.5 seconds to select it. If a word does not match, use **Review my words** to revisit the same backup and try again. When restoring an existing backup, review the words on the OLED if fitted and confirm the import with BOOT. Existing configured WiFi passwords and wallets carry over on upgrade.
4. On devnet, tap "Get 1 test SOL", then Send. Hold BOOT when the page asks.

## Try it without a board

```bash
tools/run.sh            # first start: factory password, no wallet
tools/run.sh --no-oled  # try the board-only setup
tools/run.sh --ready    # skip setup, PIN 123456
```

This starts `tools/simulator.py`, a stand-in for the device and a small fake Solana network, and opens two pages: the wallet (`http://localhost:8080`) and a virtual board with its screen and BOOT button (`http://localhost:8080/device`, hold the button with the mouse or the space bar).

## Source

```
src/main.cpp      access point, mDNS, boot
src/solder.cpp    settings, PIN session, button and LED, building and signing transactions
src/web.cpp       HTTP API, captive portal, request checks
src/display.cpp   optional OLED
web/index.html    the wallet page (packed into src/web_ui.h)
tools/            simulator and run script
docs/SECURITY.md  what Solder protects against, and what it does not
```

| Endpoint | |
| --- | --- |
| `GET /api/state` | wallet, network, password status |
| `POST /api/ap-password` | set the WiFi password, then restart |
| `POST /api/setup/generate`, `/api/setup/words`, `/api/setup/finish`, `/api/setup/cancel` | create or restore using a per-setup token |
| `POST /api/setup/check` | OLED build: start the paper backup check or read its status using the setup token |
| `POST /api/unlock`, `/api/lock` | PIN session |
| `POST /api/send`, `/api/sign`, `/api/wipe` | create a request for the button |
| `GET /api/request`, `POST /api/request/reject` | follow or cancel it; a signed send returns the transaction |
| `POST /api/network` | devnet, mainnet or your own RPC (used by the phone) |

## Security

The default builds use a six-digit PIN and ordinary ESP32 flash. Physical flash protection requires explicit provisioning, including encrypted NVS. The OLED build lets you verify the full recipient, token mint and message on the device. Read the [security notes](docs/SECURITY.md) before funding a wallet.

## License

MIT
