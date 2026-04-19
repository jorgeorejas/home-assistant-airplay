# Phase 0 report — DenAir viability PoC

**Date:** 2026-04-19
**Branch:** `phase0-raop`
**Board:** Home Assistant Voice PE (NC-VK-9727), MAC `d8:3b:da:75:03:f4`, ESP32-S3R8 v0.2, 8 MB octal PSRAM @ 80 MHz, 16 MB flash
**Toolchain:** ESP-IDF v5.4.1 (Python 3.13 venv), macOS 15.4

## Verdict

**GREEN.** AirPlay 2 via `rbouteiller/airplay-esp32` fits comfortably on this hardware with the DenAir additions. Proceed to Phase 1.

## Gate

| Metric | Target | Actual | Pass |
|---|---|---|---|
| Free internal heap during active AirPlay session | ≥100 KB | **168 KB** (~73 KB largest contiguous block) | ✅ |
| Free PSRAM during active stream | log only | 5.4 MB free | ✅ |
| App binary fits app partition | ≤3 MB | 1.4 MB (55 % partition free) | ✅ |
| iOS discovers via mDNS | yes | yes — `DenAir` visible in Control Center, Mac client 192.168.1.45 connected | ✅ |
| WiFi association (with fallback) | yes | SSID_1 miss → SSID_2 connect in 16 s, rssi −45 dBm | ✅ |
| AIC3204 DAC comes up | yes | `AIC3204 online @ 0x18`, full ESPHome-parity init sequence completes | ✅ |
| Latency | ≤300 ms (AirPlay 2 relaxed budget) | *not measured at bench yet* — stream plays, no dropouts observed | ⏳ |
| Audio at the 3.5 mm jack | yes | *not yet verified with cable — codec-side only* | ⏳ |

## Evidence (from hardware boot log, 2026-04-19 13:54:45)

```
I (3632) AIC3204: AIC3204 online @ 0x18
I (3632) HA-Voice-PE: HA Voice PE initialized (DenAir)
...
I (16102) HA-Voice-PE: WiFi fallback: switching to SSID='Wireless_CASA' (slot 2)
I (16182) wifi:connected with Wireless_CASA, aid = 1, channel 6, BW20, rssi: -45
I (17652) wifi: Got IP: 192.168.1.57
I (17672) main: Starting AirPlay services...
I (17692) rtsp_server: RTSP server listening on port 7000
I (17702) main: AirPlay ready
I (31122) HA-Voice-PE: WiFi fallback: connected on SSID='Wireless_CASA', supervisor stopping
I (51852) rtsp_server: New client connected
I (51852) rtsp_server: Client IP: 192.168.1.45
I (54732) rtsp_handlers: RECORD received - starting playback, stream_paused was 0
I (54732) audio_rt: Free heap: 168311 internal (largest block 73728), 5487504 SPIRAM
I (55462) rtsp_handlers: Configured codec: ALAC (ct=2, sr=44100, spf=352)
I (58582) rtsp_handlers: Album = NO ME QUIERO MORIR NUNCA, Artist = RATA,
                         Title  = DEJARSE LOS NUDILLOS
```

Track changed to `TIPOS DUROS` ~90 s into the session. Artwork 180 KB received twice. No panics, no WDT resets, no `ERROR`-level log lines.

## What works

- **Build pipeline**: `tools/denair-build.sh` handles ESP-IDF v5.4.1 env (Python 3.13 shim, cmake/ninja from Homebrew, SDKCONFIG_DEFAULTS plumbing, port auto-discovery).
- **`ha_voice_pe` board target**: I2C master on GPIO5/6, GPIO47 amp-enable output, AIC3204 driver registered, full ESPHome-parity codec init sequence (PLL, NDAC/MDAC/DOSR, I2S 32-bit, output routing to both HPL/R and LOL/R, 2.5 s HP soft-step).
- **WiFi compile-time config**: `wifi_config.h` seeded into NVS on every boot; two-SSID fallback supervisor flips from SSID_1 to SSID_2 after 15–25 s if the primary isn't visible (validated live — SSID_1 Asturias, SSID_2 Madrid, connected on the fallback).
- **AirPlay 2 discovery + control**: mDNS advertises, iOS Control Center discovers, RTSP handshake succeeds, ALAC stream negotiation works, metadata + artwork flow, PTP peer negotiation succeeds.

## What Phase 0 did NOT prove (deferred to Phase 1)

1. **Audio actually out of the 3.5 mm jack.** The AIC3204 registers are all set but we haven't put a scope or headphones on the output yet. The ESPHome register sequence is known-good on this exact board, so risk here is low but not zero.
2. **Soak-stability.** The observed 168 KB heap number is a snapshot a few seconds after stream start. The plan called for 10 min idle + 10 min playing + 10 min pause/resume via `tools/heap_probe.py` — that CSV isn't captured yet. Everything we've seen suggests it'll stay stable (no leak signals, no fragmentation warnings), but the soak isn't on record.
3. **End-to-end latency measurement.** Anecdotally the stream starts within a second or two of tap-to-play; no scope/loopback measurement yet.
4. **Jack-detect auto-switch (PRD §5.3).** GPIO17 wired but not yet read — internal amp is permanently on (GPIO47 = high). Phase 1 item.
5. **Encoder volume (PRD §5.4).** GPIO16/18 defined in `iot_board.h` but no encoder task yet. Phase 1 item.
6. **LED engine (PRD §5.5).** WS2812B on GPIO21 — untouched. Phase 2 item.

## Known quirks / follow-ups

- **Format-warning relaxation.** `CMakeLists.txt` adds `-Wno-error=format -Wno-format` project-wide because airplay-esp32 was cut against ESP-IDF 5.3 and 5.4 widened some log-macro integer types. Should revisit when upstream catches up.
- **`-dirty` ESP-IDF tag.** Our ESP-IDF clone is missing one submodule (`components/cmock/CMock/vendor/c_exception`) that repeatedly SSL-failed during init — only affects the IDF test suite, not builds.
- **Console path.** UART_DEFAULT primary + USB_SERIAL_JTAG secondary is the config that actually produces log output on this board. USB_CDC primary was silent in testing even with the Kconfig choice correctly applied — reason unclear, worth a revisit if we ever need USB_CDC for a specific reason.
- **Kconfig choice gotcha.** `idf.py set-target` discovers its own defaults; without `SDKCONFIG_DEFAULTS` in the env it locks in choice options before our overlay applies. `tools/denair-build.sh` now plumbs the env var from the start; any future automation that calls `idf.py` directly needs to do the same.
- **Captive portal still active.** Upstream's `ESP32-AirPlay-Setup` AP is still advertised at 192.168.4.1 until STA connects; after association the AP goes away. We could disable it entirely in a Phase 3 sweep, but it's useful as a "something's wrong" recovery path for now.

## Next: Phase 1 scope (per the plan)

- Jack-detect ISR on GPIO17 (200 ms debounce), toggle GPIO47 amp enable based on plug state without interrupting the stream.
- Encoder on GPIO16/18 → AIC3204 DAC volume register (0.5 dB steps); push/long-press → mute / mode cycle.
- Persist last volume + LED mode in NVS.
- Basic VU-style feedback on the 12-LED ring as placeholder for the full LED engine.
- Revisit the bench soak + latency measurements to close the two ⏳ rows in the gate table.
