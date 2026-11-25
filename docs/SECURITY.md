# Security notes

Solder is a do-it-yourself wallet. This page says plainly what it protects you from, where it is weaker than a commercial hardware wallet, and how to close the gaps.

## What stays on the device

- The backup words and the private key are created on the ESP32 from its hardware random number generator (with WiFi on, which feeds the generator).
- They are stored encrypted with AES-256-GCM. The key comes from your PIN through PBKDF2-HMAC-SHA256 with 20 000 rounds and a random salt.
- After unlocking, the signer keeps its live key in RAM. Locking, session replacement and erasure clear the application's signing key and pending requests. The crypto library and browser can create temporary copies in memory.
- The firmware suppresses ESP-IDF crash dumps that could persist task stacks. On startup it clears any old data in the dedicated crash-dump partition before opening the wallet API.
- The API has no way to read the key or the words after setup.
- Solder has no route to the internet: it only runs its own access point. The phone fetches balances and broadcasts signed transactions, so the device that holds the key never talks to a server.

## What the button gives you

Every transaction and message signature requires a fresh 1.5-second BOOT hold. On OLED, tap through each page and hold on the last page. The screen shows the amount, complete recipient, and full mint for a token transfer. Messages use multiple pages and show non-printable and non-ASCII bytes as hex escapes. The wallet expires a request after 45 seconds without physical review activity. A long review can outlive the supplied blockhash.

## Threats and answers

| Threat | Answer |
| --- | --- |
| Someone knows another board's setup password | Each board creates its own random pairing password, displays it locally and requires the owner to replace it. Wallet setup waits until the new WiFi configuration is active |
| Someone on Solder's WiFi calls the API | Wallet actions require a PIN session. The address and RPC URL are available to the authenticated session. An in-progress setup uses a separate short-lived token. An imported phrase also requires BOOT confirmation |
| A website you visit calls Solder from your browser | JSON-only API (no simple form posts), custom session header, no CORS answers, so the browser blocks it |
| DNS rebinding (a website points its domain at Solder's IP) | Solder only answers requests addressed to `192.168.4.1` or `solder.local` |
| PIN guessing | Attempts are recorded before PIN verification, so a power interruption during verification can consume a try. After five consecutive failures, each further attempt requires a fresh BOOT hold and release tied to the originating tab and candidate PIN. The tenth wrong PIN logically removes the wallet. Its backup words can restore it |
| Stolen device, flash read out | The PIN encrypts the wallet. A six-digit PIN is susceptible to offline guessing when someone obtains the encrypted wallet blob from ordinary NVS |
| Hacked browser changes the recipient or amount | Solder builds the transaction and the OLED displays the full transfer details. Check every page before approval. When receiving, compare the complete address on the unlocked device with the address you share |
| Hacked phone substitutes a blockhash from another cluster | The offline device cannot verify which Solana cluster issued a blockhash. A valid mainnet blockhash can make a transfer executable on mainnet even if the browser says devnet. Use a separate wallet for testing |
| Sniffing Solder's WiFi | WPA2 with your own password. The API is plain HTTP inside that link |

## Known limits

- **No secure element.** A plain ESP32 keeps the encrypted secret in normal flash. Physical access plus a flash dump allows an offline PIN search.
- **Without the OLED, the browser is the display.** The button proves that a person approved, not what they approved. Treat the board-only build as a demo or a small hot wallet.
- **Backup words during setup.** With OLED, newly generated words appear only on the device. Before saving the wallet, the firmware checks three word positions using the OLED and BOOT button. The browser receives only the check status. The board-only build also sends the generated words to the browser. Restoring on either build sends the words and PIN from the browser to the board. An untrusted phone can copy anything entered into it.
- **Plain HTTP.** Solder relies on its WPA2 link and session token. A compromised endpoint or an attacker able to intercept that link can see setup secrets, PINs and session tokens. The custom RPC must use HTTPS. Its operator can still misreport balances, network identity or confirmation status.
- **Erasure and RAM.** Removing the NVS entry revokes access through the firmware. A copied flash image, possible historical flash records and working buffers in dependencies are outside that guarantee.
- **Simple transactions only.** Solder builds the transactions itself (SOL and token transfers). It does not yet sign arbitrary transactions from dApps; when it does, it must decode them on the device and show them on the screen.

## For real funds

1. Build with the OLED (`esp32dev-oled`).
2. Provision release-mode flash encryption, secure boot, and NVS encryption with an encrypted NVS key partition. The supplied PlatformIO builds do not provision these hardware protections. Verify the installed configuration on the target board. Provisioning changes eFuses and affects future firmware updates. See Espressif's [flash encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/flash-encryption.html) and [NVS encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_encryption.html) guides.
3. Keep the backup words offline; they restore the wallet on any Phantom-compatible wallet.
4. Next step on the hardware side: an ATECC608 secure element to hold the PIN-check counter and a key-wrapping key, so even a flash dump plus unlimited time does not give the secret.
