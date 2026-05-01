# Home Assistant AirPlay

**AirPlay 2 receiver firmware for the [Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/).** Turns the device into a hi-fi-class wireless audio endpoint that feeds a 3.5 mm jack, with a 12-LED ring that reacts to the music and follows the album artwork.

> **About the hardware** —
> [product page](https://www.home-assistant.io/voice-pe/) ·
> [user docs](https://voice-pe.home-assistant.io/) ·
> [stock firmware source](https://github.com/esphome/home-assistant-voice-pe) ·
> [buy from Home Assistant](https://www.home-assistant.io/voice-pe/#order)

```
  iPhone / Mac ─── WiFi ──▶ HA Voice PE (ESP32-S3 + TLV320AIC3204)
                                   │
                                   ├──▶ 3.5 mm jack ─ Denon AUX / any amp
                                   │
                                   └──▶ 12-LED WS2812B ring
                                          beat-pulse, hue from album art,
                                          volume bar, connection sweep
```

Status: **Phases 1 + 2 (jack-detect) shipped** ([CHANGELOG](CHANGELOG.md)). Phase 0 viability gate passed with 168 KB free internal heap under active AirPlay streaming — well above the 100 KB target ([Phase 0 report](docs/phase0-report.md)).

## Features

- **AirPlay 2 receive**, discovered natively in iOS / macOS Control Center as a classic audio receiver. ALAC + AAC codecs, 44.1 and 48 kHz.
- **Hi-fi audio out** via the 3.5 mm jack, with **automatic jack-detect** that mutes the internal amp when headphones / aux cable is plugged in (5 s cyan/amber LED indicator confirms the new output destination).
- **Web dashboard** at `http://<friendly-name-slug>.local/` — real-time **now-playing** card with album art, **15-band parametric EQ** (5 presets) running as software DSP in the playback task, status, device rename, restart. Embedded in flash, no SPIFFS dependency.
- **Live log viewer** at `/logs` — WebSocket-streamed device logs with level coloring, filter, pause, autoscroll.
- **MagSafe-style connection chime** plays when an AirPlay client connects.
- **Rotary encoder** for volume (±3 dB per detent), persistent across reboots.
- **Center button** — short press pauses, double/triple click attempts next/prev, long press toggles local mute. See [`components/ha_airplay_ui/README.md`](components/ha_airplay_ui/README.md) for the full map.
- **Side slide switch** on GPIO3 gates the *decorative* LED renders only (PLAYING beat-pulse, IDLE breath). Utility overlays — mute, volume bar, connection sweep — render in either position so essential feedback is always visible.
- **Audio-reactive LED ring** — bass-band beat detector flashes the ring on each detected beat. Base colour follows the album artwork when the image has a dominant hue. Soft amber idle breath when nothing is playing.
- **Friendly mDNS hostname slug** — UTF-8 friendly name (`Altavoces Salón`) auto-folds to a browser-safe slug (`altavoces-salon.local`). Editable from the dashboard.
- **OTA over HTTP** — `POST /api/ota/update` accepts a raw binary; firmware is buffered in PSRAM, SHA-256 verified, written to the inactive partition, and marked valid only after 60 s of healthy network on the next boot.
- **Compile-time WiFi credentials** in a git-ignored `wifi_config.h` with **automatic two-SSID fallback**. Credentials seeded from the header only when NVS is empty, so renaming the device or changing networks at runtime sticks across reboots.
- **No cloud, no phone app, no Home Assistant integration required.** The device is a standalone AirPlay receiver.

## Quick start

1. Install ESP-IDF v5.4.1 and the Python 3.13 shim — one-time setup detailed in [docs/build.md](docs/build.md).
2. Copy the WiFi template:
   ```bash
   cp wifi_config.h.example wifi_config.h
   # edit WIFI_SSID_1 / WIFI_PASSWORD_1
   ```
3. Build and flash:
   ```bash
   tools/ha-airplay-build.sh build
   tools/ha-airplay-build.sh flash
   ```
4. On iPhone or Mac, open Control Center → AirPlay → **Home Assistant AirPlay**. Play music.
5. Browse to `http://<your-device-slug>.local/` for the dashboard, or use the `/api` endpoints — see [docs/dashboard.md](docs/dashboard.md).

After the first flash, future updates can go over the air without USB:
```bash
tools/ha-airplay-build.sh build
curl -X POST --data-binary @build/airplay2-receiver.bin \
  http://<your-device-slug>.local/api/ota/update
```

## Repo layout

```
.
├── main/                        ← airplay-esp32 upstream (RTSP / HAP / audio / PTP / mDNS)
├── components/
│   ├── boards/ha_voice_pe/      ← pin map, I²C bus, WiFi seed + fallback
│   ├── dac_tlv320aic3204/       ← Home Assistant AirPlay DAC driver (register sequence port)
│   ├── ha_airplay_leds/             ← WS2812B render loop + audio tap + beat detector
│   ├── ha_airplay_ui/               ← encoder, button (multi-click), decorative-LED slide gate
│   ├── ha_airplay_artwork/          ← tjpgd artwork decoder → LED base hue
│   ├── dac/ dac_tas57xx/ …      ← upstream DAC abstraction + siblings
│   └── boards/ (other) …        ← upstream reference boards
├── docs/
│   ├── architecture.md          ← system diagram + module graph
│   ├── hardware.md              ← pin map, schematic/datasheet
│   ├── build.md                 ← toolchain setup + build/flash recipes
│   ├── led-ux.md                ← LED state machine spec
│   ├── phase0-report.md         ← viability gate (GREEN)
│   ├── phase1-report.md         ← Phase 1 shipped features
│   └── UPSTREAM-README.md       ← airplay-esp32's README, preserved
├── sdkconfig.defaults.ha_voice_pe   ← Home Assistant AirPlay config overlay
├── tools/
│   ├── ha-airplay-build.sh          ← idf.py wrapper (PATH, SDKCONFIG_DEFAULTS, port discovery)
│   └── heap_probe.py            ← Phase 0 heap-gate harness
└── wifi_config.h.example        ← template for the compile-time WiFi config
```

Every Home Assistant AirPlay component has its own `README.md` explaining what it does, why, and how it's wired.

## Hardware

Target: **[Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/)** (NC-VK-9727). No physical modifications required. Full pin map and schematic references in [docs/hardware.md](docs/hardware.md).

- Product and purchase: https://www.home-assistant.io/voice-pe/
- User documentation: https://voice-pe.home-assistant.io/
- Hardware specs: https://voice-pe.home-assistant.io/hardware/
- Stock (ESPHome) firmware source: https://github.com/esphome/home-assistant-voice-pe
- Reinstall the stock firmware: https://esphome.github.io/home-assistant-voice-pe/

## Documentation index

| File | Topic |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Component graph, data flow, concurrency |
| [docs/hardware.md](docs/hardware.md) | GPIO pin map and schematic/datasheet links |
| [docs/build.md](docs/build.md) | Toolchain setup, build / flash / monitor recipes |
| [docs/dashboard.md](docs/dashboard.md) | Web UI + HTTP API reference |
| [docs/led-ux.md](docs/led-ux.md) | LED state machine spec |
| [docs/phase0-report.md](docs/phase0-report.md) | Viability gate results |
| [docs/phase1-report.md](docs/phase1-report.md) | Phase 1 shipped features |
| [docs/review/](docs/review/) | Multi-agent project review (architecture, performance, reliability, UX, security) and the punch list that drove the Sprint A–D follow-ups |
| [CHANGELOG.md](CHANGELOG.md) | Versioned change log |
| [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md) | airplay-esp32 README (unmodified) |
| `components/boards/ha_voice_pe/README.md` | Board layer |
| `components/dac_tlv320aic3204/README.md` | DAC driver |
| `components/ha_airplay_leds/README.md` | LED engine + audio tap + beat detector |
| `components/ha_airplay_ui/README.md` | Encoder, button, slide-switch, jack-detect |
| `components/ha_airplay_artwork/README.md` | Artwork JPEG decoder → hue |

## Status & roadmap

- **Phase 0** ✅ GREEN. AirPlay 2 stack fits on ESP32-S3 with ~68 KB heap margin.
- **Phase 1** ✅ DONE. Audio out, encoder volume, button controls, audio-reactive LED ring with artwork hue, WiFi fallback, compile-time credentials.
- **Phase 2** 🛠 partially shipped: ✅ jack-detect (GPIO17 → GPIO47 amp toggle, 5 s output-destination LED indicator). 📅 still open: spectrum LED mode, long-press LED mode cycle, persistent LED mode in NVS.
- **Phase 3** 📅 planned. OTA polish, ≤8 s boot, multi-week soak test.

Beyond the original PRD, **Sprints A–D** (driven by [`docs/review/`](docs/review/)) added: software 15-band parametric EQ, embedded web dashboard with album art, live log viewer, friendly mDNS slug, MagSafe-style connection chime, factory-seed-once credential model, doc cleanup, and a number of reliability fixes (jitter tolerance, OTA rollback, RTSP recovery, DMAP depth limit). See [CHANGELOG.md](CHANGELOG.md) for the per-version detail.

## Built on

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) — the AirPlay 2 receiver core. Home Assistant AirPlay is a friendly fork that keeps `main/` unmodified and lives entirely in `components/`.
- [ESPHome's aic3204 component](https://github.com/esphome/esphome/tree/dev/esphome/components/aic3204) — source of the DAC register sequence.
- [shairport-sync](https://github.com/mikebrady/shairport-sync) and [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver) — protocol reverse-engineering that makes any of this possible.

## Licence

Inherits the upstream airplay-esp32 licence: **non-commercial use only**. See [LICENSE](LICENSE). Home Assistant AirPlay is a personal / hobbyist project; commercial redistribution needs explicit permission from the upstream author.
