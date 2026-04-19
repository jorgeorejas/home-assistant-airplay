# DenAir

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

Status: **Phase 1 complete** ([report](docs/phase1-report.md)). Phase 0 viability gate passed with 168 KB free internal heap under active AirPlay streaming — well above the 100 KB target ([Phase 0 report](docs/phase0-report.md)).

## Features

- **AirPlay 2 receive**, discovered natively in iOS / macOS Control Center as a classic audio receiver. ALAC + AAC codecs, 44.1 and 48 kHz.
- **Hi-fi audio out** via the 3.5 mm jack, or internal speaker when nothing is plugged in.
- **Rotary encoder** for volume (±3 dB per detent). Volume is persistent across reboots.
- **Center button** — short press pauses, double/triple click attempts next/prev, long press toggles local mute. See [`components/denair_ui/README.md`](components/denair_ui/README.md) for the full map.
- **Side slide switch** on GPIO3 is repurposed as the LED ring on/off (DenAir doesn't use the microphone).
- **Audio-reactive LED ring** — bass-band beat detector flashes the ring on each detected beat. Idle state is a soft amber breath. Base colour follows the album artwork when the image has a dominant hue.
- **Compile-time WiFi credentials** in a git-ignored `wifi_config.h` with **automatic two-SSID fallback** — ideal for carrying the device between homes.
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
   tools/denair-build.sh build
   tools/denair-build.sh flash
   ```
4. On iPhone or Mac, open Control Center → AirPlay → **DenAir**. Play music.

## Repo layout

```
.
├── main/                        ← airplay-esp32 upstream (RTSP / HAP / audio / PTP / mDNS)
├── components/
│   ├── boards/ha_voice_pe/      ← pin map, I²C bus, WiFi seed + fallback
│   ├── dac_tlv320aic3204/       ← DenAir DAC driver (register sequence port)
│   ├── denair_leds/             ← WS2812B render loop + audio tap + beat detector
│   ├── denair_ui/               ← encoder, button (multi-click), LED on/off slide
│   ├── denair_artwork/          ← tjpgd artwork decoder → LED base hue
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
├── sdkconfig.defaults.ha_voice_pe   ← DenAir config overlay
├── tools/
│   ├── denair-build.sh          ← idf.py wrapper (PATH, SDKCONFIG_DEFAULTS, port discovery)
│   └── heap_probe.py            ← Phase 0 heap-gate harness
└── wifi_config.h.example        ← template for the compile-time WiFi config
```

Every DenAir component has its own `README.md` explaining what it does, why, and how it's wired.

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
| [docs/led-ux.md](docs/led-ux.md) | LED state machine spec |
| [docs/phase0-report.md](docs/phase0-report.md) | Viability gate results |
| [docs/phase1-report.md](docs/phase1-report.md) | Phase 1 shipped features |
| [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md) | airplay-esp32 README (unmodified) |
| `components/boards/ha_voice_pe/README.md` | Board layer |
| `components/dac_tlv320aic3204/README.md` | DAC driver |
| `components/denair_leds/README.md` | LED engine + audio tap + beat detector |
| `components/denair_ui/README.md` | Encoder, button, mute slide |
| `components/denair_artwork/README.md` | Artwork JPEG decoder → hue |

## Status & roadmap

- **Phase 0** ✅ GREEN. AirPlay 2 stack fits on ESP32-S3 with ~68 KB heap margin.
- **Phase 1** ✅ DONE. Audio out, encoder volume, button controls, audio-reactive LED ring with artwork hue, WiFi fallback, compile-time credentials.
- **Phase 2** 🛠 in scope. Jack-detect auto-switch (GPIO17 → GPIO47 amp enable), spectrum LED mode, long-press mode cycle.
- **Phase 3** 📅 planned. OTA polish, ≤8 s boot, multi-week soak test.

## Built on

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) — the AirPlay 2 receiver core. DenAir is a friendly fork that keeps `main/` unmodified and lives entirely in `components/`.
- [ESPHome's aic3204 component](https://github.com/esphome/esphome/tree/dev/esphome/components/aic3204) — source of the DAC register sequence.
- [shairport-sync](https://github.com/mikebrady/shairport-sync) and [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver) — protocol reverse-engineering that makes any of this possible.

## Licence

Inherits the upstream airplay-esp32 licence: **non-commercial use only**. See [LICENSE](LICENSE). DenAir is a personal / hobbyist project; commercial redistribution needs explicit permission from the upstream author.
