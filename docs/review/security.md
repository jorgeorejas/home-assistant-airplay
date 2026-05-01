# Security review

**Scope:** LAN-local threat model — anyone on the residential WiFi can reach every TCP/UDP port on the device. No internet exposure assumed.
**Date:** 2026-05-01

## TL;DR
- **HIGH** — Unauthenticated `POST /api/ota/update` accepts any signed-by-nobody firmware over plain HTTP. The "validation" only verifies the SHA-256 *the attacker put there themselves*. Any LAN device can permanently take over the box (persistent backdoor that survives reboot).
- **HIGH** — Plaintext `wifi_config.h` in the working tree contains a real WPA2 passphrase (`OrejasRojo` / `14860242`). It is `.gitignore`d and not in git history, but it lives un-redacted next to source you may share, paste into LLMs, or upload as a build log. Treat it as already-leaked and rotate.
- **MED** — AirPlay HAP pair-setup uses a hardcoded PIN (`"0000"` non-transient, `"3939"` transient) and `hap_pair_verify_m3_raw` explicitly skips the client signature check. Anyone on the LAN can pair and stream.
- **MED** — `parse_dmap_metadata()` recurses without depth limit on attacker-supplied container tags; a crafted RTSP `SET_PARAMETER` body blows the 8 KB rtsp task stack.
- **LOW/MED** — `/ws/logs` WebSocket is unauthenticated and streams every `ESP_LOG*` line on the LAN, including the WiFi SSID at boot and connection-reason codes.

## Findings

### [HIGH] Unauthenticated, unsigned OTA over HTTP
**Where:** `main/network/web_server.c:265-289` (`ota_update_handler`), `main/network/ota.c:18-58` (`ota_validate_image`), `sdkconfig` (`# CONFIG_SECURE_BOOT is not set`, `# CONFIG_FLASH_ENCRYPTION_ENABLED is not set`).
**Threat:** A neighbour's compromised IoT device, a guest's phone, or a malicious app on a phone joined to the WiFi can `curl -X POST --data-binary @evil.bin http://<device>/api/ota/update` and get persistent code execution on a box wired into the home audio system and on the same VLAN as everything else. Nothing requires auth, nothing requires user interaction at the device.
**Current state:** `ota_validate_image` checks the ESP image magic byte, segment count, and *if the image itself claims `hash_appended`*, verifies the SHA-256 trailer matches the bytes that precede it. That is a self-consistency check (any attacker can compute SHA-256), not a signature. With Secure Boot V2 disabled, ESP-IDF's `esp_ota_end()` also won't verify a signature. The bootloader will happily run the new image on next reboot.
**Suggested fix:** Cheapest win: gate `/api/ota/update` behind a token kept in NVS (set once via `idf.py monitor` or first-boot AP) and rate-limit failures. Right next step: enable Secure Boot V2 and sign images — the keys plumbing is already half-there (`CONFIG_SECURE_BOOT_V2_PREFERRED=y`). Long term: `app_check_image_signature` against an embedded public key inside `ota_validate_image` before `esp_ota_begin`.
**Effort:** S for the token, M for full signed-OTA (one-way: signing keys must be generated and protected).

### [HIGH] Plaintext WiFi PSK in the source tree
**Where:** `wifi_config.h:14-15`, `.gitignore:18`, `partitions.csv → nvs (unencrypted)`, `sdkconfig` (`# CONFIG_NVS_ENCRYPTION is not set`).
**Threat:** Two leaks in one. (1) The repo holds the home WPA2 PSK in plaintext as a checked-out (but ignored) C header — easy to accidentally `cat` into an LLM, paste into an issue, or include in a `tar` you mail yourself. (2) The same PSK is written to NVS unencrypted; anyone who pulls the flash off the chip (or grabs a built `.bin` from `build/` and runs `espefuse`/`esptool` against a saved partition image) recovers it. The `nvs_key` partition exists in `partitions.csv:6` as if NVS encryption were planned, but the corresponding Kconfig is off, so it's dead weight.
**Suggested fix:** Rotate the PSK. Stop seeding `wifi_config.h` from the repo — let the captive portal be the only path to enter credentials, and on first boot generate a unique AP password derived from MAC/efuse. If you keep `wifi_config.h` for dev, replace its real values with `"REPLACE_ME"` and load real values from a file outside the repo (e.g. `~/.homeassistant-airplay/wifi_config.h` symlinked, or via env at flash time). Either enable `CONFIG_NVS_ENCRYPTION` (the `nvs_key` partition is already reserved) or accept the physical-attacker risk explicitly.
**Effort:** S to rotate + redact, M to wire up NVS encryption (needs flash encryption first to protect the key).

### [MED] AirPlay pairing uses a fixed PIN and skips signature verification
**Where:** `main/hap/hap_pair_setup.c:55` (`const char *password = transient ? "3939" : "0000";`), `main/hap/hap_pair_verify.c:296` (`ESP_LOGW(TAG, "Skipping signature verification (transient pairing)");`), and `hap_pair_setup_m5` at `main/hap/hap_pair_setup.c:155-211` decrypts the M5 encrypted payload but never validates the Ed25519 signature inside the sub-TLV.
**Threat:** Any LAN attacker can pair as a "trusted" controller and push audio / metadata / artwork to the speaker, masquerade as the legitimate pairing on later sessions, and reach the post-pair RTSP surface (which is where most of the parsing code lives — see DMAP issue below).
**Current state:** Apple's HAP spec expects the PIN to be displayed on the accessory and entered by the user; here it's a constant, so SRP-6a authenticates the password *the attacker already knows*. Pair-verify's "raw" path simply decrypts the client signature with AES-CTR and never calls `crypto_sign_verify_detached`.
**Suggested fix:** Given this is a hi-fi receiver with no display, a static PIN is unavoidable — but at least make it derivable from device MAC/serial and printed on a label, not committed to source as `"0000"`. For pair-verify, plumb through the iOS controller's long-term public key from a stored pairing record and call `crypto_sign_verify_detached` instead of logging "skipping". If you want to keep the AirPlay surface fully open on your own LAN, document that explicitly.
**Effort:** S to randomize the PIN per device; M to implement persistent pairings + signature verification.

### [MED] `parse_dmap_metadata` recurses on attacker-controlled container tags
**Where:** `main/rtsp/rtsp_handlers.c:1282-1339`.
**Threat:** A paired (or pair-bypassed, see above) AirPlay client sends a `SET_PARAMETER` with `Content-Type: application/x-dmap-tagged` whose body is a `mlit` container nested 1000 deep. Each recursion adds a stack frame; the RTSP task stack overflows and the device hard-crashes (DoS) or, in the worst case, corrupts adjacent stacks on FreeRTOS.
**Current state:** No depth counter. `mlit`, `cmst`, `mdst` recurse unconditionally with `parse_dmap_metadata(data + pos, item_len, meta);`.
**Suggested fix:** Pass an `int depth` parameter, bail at `depth > 8`. One-line guard.
**Effort:** S.

### [MED] Path-traversal hardening in `/api/fs/*` is brittle but currently OK
**Where:** `main/network/web_server.c:355-369` (`is_path_allowed`, `ALLOWED_PREFIXES = {"/spiffs/"}`).
**Threat:** None today, but the check is `strncmp(path, "/spiffs/", 8) == 0 && !strstr(path, "..")` — that rejects `..` substrings anywhere, which means a legit filename containing `..` is rejected (cosmetic) but also means a Unicode-encoded traversal isn't possible because the filesystem is SPIFFS (no real directories). The risk is regression: if someone later adds a second prefix like `/littlefs/` or mounts SPIFFS at `/`, the `..` check no longer suffices because `path` could be e.g. `/spiffs/foo/%2e%2e/...` — wait, `httpd_query_key_value` URL-decodes, so `%2e%2e` → `..` is caught. OK today.
**Current state:** Good enough for SPIFFS-only. Note that `fs_list` accepts an attacker-controlled `dir_path[64]` and silently truncates if longer — not exploitable, just a UX wart.
**Suggested fix:** Add a comment in `is_path_allowed` warning future-you that the `..` check assumes URL-decoded input from `httpd_query_key_value`. If you ever add a writable mount outside SPIFFS, switch to canonicalize-then-prefix-check rather than substring-rejection.
**Effort:** S (defensive comment), no immediate action.

### [LOW] `/api/system/restart` and `/api/wifi/config` are LAN-reachable kill switches
**Where:** `main/network/web_server.c:333-348` (restart), `main/network/web_server.c:172-221` (`wifi_config_handler`).
**Threat:** Any LAN device can `POST /api/system/restart` in a loop → permanent denial of service. Worse: any LAN device can `POST /api/wifi/config '{"ssid":"attackerAP","password":"x"}'`, which `settings_set_wifi_credentials()` writes to NVS *and then immediately reboots* (web_server.c:200-203). After reboot the device joins the attacker's open AP — enabling traffic redirection or just stranding the speaker until a factory reset. Note `settings_set_wifi_credentials` validates length but accepts any printable garbage as a password, including empty.
**Current state:** Same authentication situation as OTA: none.
**Suggested fix:** If you add the OTA token from finding #1, gate these two endpoints with the same token. If you don't, at least require the WiFi-config POST to come from the AP interface (192.168.4.0/24) — i.e. only accept credential-changes when the device is *not* yet associated, or when the request arrives on the AP netif. Reject the request when STA is up.
**Effort:** S.

### [LOW] `/ws/logs` leaks runtime data to anyone on the LAN
**Where:** `main/network/log_stream.c:108-134` (`ws_log_handler` — no auth, only a 3-client cap).
**Threat:** A LAN observer connects to `ws://<device>/ws/logs` and sees the WiFi SSID at boot (`"Connecting to WiFi: %s"` in `wifi.c:296`), client IPs, RTSP request lines (which include client UUIDs and now-playing metadata), volume changes, and HAP debug. Not catastrophic in your stated threat model, but useful for fingerprinting and watching when nobody is home (`Title = ...` only flows when music plays).
**Current state:** Whoever asks, gets logs.
**Suggested fix:** Same token gate, or scrub the PSK / SSID / pairing keys from log lines before they enter the ring buffer (`log_stream.c:79-102`). At minimum, change `ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);` (`wifi.c:296`) to log only the first few chars, and demote pairing-error logs to `ESP_LOGD`.
**Effort:** S for log scrubbing, S for adding a token check to the WS upgrade.

## Acceptable risks (called out, but won't fix)
- **No HTTPS on the local web server.** mDNS-discovered IPs and self-signed certs would be a UX disaster on a residential LAN. The OTA token + Secure Boot above mitigates the worst-case anyway.
- **AirPlay 1 RSA key is a published Shairport key**, so the AirPlay-1 fallback path provides no real client authentication. This is true of every open-source AirPlay receiver and is part of the AirPlay-1 deal.
- **mDNS TXT records are public on the LAN.** Anyone can enumerate the device's features/capabilities. By design.
- **Captive portal at `192.168.4.1` is correctly torn down** when STA associates (`wifi.c:114-119` switches to `WIFI_MODE_STA`; `main.c:152-155` stops the DNS server). Not a finding — flagged because the prompt asked.

## Healthy
- `wifi_config.h` is correctly `.gitignore`d and `git log --all` shows it has never been committed (verified). The leak risk is local-machine, not GitHub.
- The DMAP/metadata `memcpy`s into fixed buffers (`rtsp_handlers.c:1303-1330`) all clamp `copy_len < METADATA_STRING_MAX - 1` before copying — no obvious overrun there. Same for the bplist string extractors.
- ChaCha20-Poly1305 calls in HAP use libsodium (`crypto_aead_chacha20poly1305_ietf_decrypt`), which is constant-time and verifies the auth tag — return value is checked at every call site I saw (`hap_pair_setup.c:200`, `hap_pair_verify.c:121`).
- The captive-portal DNS responder rate-limits responses to one per packet and bounds the response length (`dns_server.c:71` `resp_len < DNS_MAX_LEN - 16`); no obvious amplification primitive even though it's UDP and unauthenticated.
