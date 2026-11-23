# Security notes

Solder is a do-it-yourself wallet. This page says plainly what it protects you from, where it is weaker than a commercial hardware wallet, and how to close the gaps.

## What stays on the device

- The backup words and the private key are created on the ESP32 from its hardware random number generator (with WiFi on, which feeds the generator).
- They are stored encrypted with AES-256-GCM. The key comes from your PIN through PBKDF2-HMAC-SHA256 with 20 000 rounds and a random salt.
- After unlocking, the key is kept in RAM only. Locking, the auto lock and a reboot clear it.
- The API has no way to read the key or the words after setup.
- Solder has no route to the internet: it only runs its own access point. The phone fetches balances and broadcasts signed transactions, so the device that holds the key never talks to a server.

## What the button gives you

Every transaction and message signature waits for the BOOT button to be held for 1.5 seconds. Malware on your phone or laptop can ask for a signature, but it cannot press the button. A request that is not approved in 45 seconds is dropped (a Solana blockhash only lives about a minute).

## Threats and answers

| Threat | Answer |
| --- | --- |
| Someone joins Solder's WiFi with the factory password | The page refuses to do anything until the password is replaced; Solder restarts with the new one |
| Someone on Solder's WiFi calls the API | Wallet actions need the session token from the PIN unlock; signing also needs the button |
| A website you visit calls Solder from your browser | JSON-only API (no simple form posts), custom session header, no CORS answers, so the browser blocks it |
| DNS rebinding (a website points its domain at Solder's IP) | Solder only answers requests addressed to `192.168.4.1` or `solder.local` |
| PIN guessing | Growing delay after each wrong PIN, erase after 10. The words still restore the wallet |
| Stolen device, flash read out | Secret is encrypted with the PIN. A 6-digit PIN can be brute-forced offline from a flash dump, so enable flash encryption (below) |
| Hacked browser shows one amount but asks Solder to sign another | Solder builds the transaction itself from the recipient and amount; the OLED build shows exactly those before you press the button |
| Hacked phone sends a bad blockhash | Worst case the transaction is rejected by the network; a blockhash can't redirect funds |
| Sniffing Solder's WiFi | WPA2 with your own password. The API is plain HTTP inside that link |

## Known limits

- **No secure element.** A plain ESP32 keeps the encrypted secret in normal flash. Physical access plus a flash dump allows an offline PIN search.
- **Without the OLED, the browser is the display.** The button proves that a person approved, not what they approved. Treat the board-only build as a demo or a small hot wallet.
- **Backup words travel over WiFi during setup** in the board-only build (the OLED build shows them only on the screen).
- **Plain HTTP.** A self-signed certificate would make browsers warn on every visit, so Solder relies on its WPA2 link and the session token instead.
- **Simple transactions only.** Solder builds the transactions itself (SOL and token transfers). It does not yet sign arbitrary transactions from dApps; when it does, it must decode them on the device and show them on the screen.

## For real funds

1. Build with the OLED (`esp32dev-oled`).
2. Enable flash encryption and secure boot in release mode (ESP-IDF `idf.py menuconfig` > Security features, or the PlatformIO `sdkconfig` options). This binds the flash content to this chip, so a dump is useless on another device.
3. Keep the backup words offline; they restore the wallet on any Phantom-compatible wallet.
4. Next step on the hardware side: an ATECC608 secure element to hold the PIN-check counter and a key-wrapping key, so even a flash dump plus unlimited time does not give the secret.
