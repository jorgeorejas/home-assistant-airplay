# DenAir

AirPlay 2 receiver firmware for the [Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/) (ESP32-S3 + TLV320AIC3204), feeding a Denon hi-fi through the 3.5 mm jack. Personal / open-source. Non-commercial use only (inherits airplay-esp32's license).

```
iPhone / Mac ── WiFi ──► HA Voice PE (ESP32-S3 + AIC3204)
                                │
                                ├── 3.5 mm jack ──► Denon AUX
                                │
                                └── 12-LED WS2812B ring ──► audio-reactive viz
                                                            (VU / Beat / Spectrum / Idle)
```

**Status: Phase 0 (viability)**. Builds green on ESP-IDF 5.4.1; AirPlay stack, DAC driver, and board init all compile. Hardware bring-up (flash + heap gate + first-sound-out) is the immediate next step.

## Repo layout

```
.
├── main/                               # airplay-esp32 application (RTSP, HAP, audio)
├── components/
│   ├── boards/ha_voice_pe/             # DenAir board init (GPIO47 amp enable, I2C bus)
│   ├── dac_tlv320aic3204/              # AIC3204 DAC driver (this project)
│   ├── dac/ dac_tas57xx/ dac_tas58xx/  # upstream DAC abstractions (kept, inactive)
│   ├── boards/{esp32-,esp32s3-}generic/# upstream reference boards (smoke-test target)
│   ├── audio-resampler/                # 44.1 kHz → 48 kHz sinc
│   ├── board_utils/ display/ spiffs_storage/
│   └── u8g2*/                          # trimmed; disabled via CONFIG_DISPLAY_ENABLED=n
├── sdkconfig.defaults                  # shared base (PSRAM, WiFi buffers, mbedTLS)
├── sdkconfig.defaults.esp32s3          # upstream baseline
├── sdkconfig.defaults.ha_voice_pe      # DenAir overlay (pin map, board selection)
├── wifi_config.h.example               # template for compiled-in WiFi credentials
├── tools/
│   ├── denair-build.sh                 # build/flash/monitor wrapper
│   └── heap_probe.py                   # Phase 0 heap-gate harness
├── docs/
│   ├── home-assistant-voice-pe-dev/    # upstream esphome/home-assistant-voice-pe (pin-map truth)
│   ├── home_assistant_voice_pe_schematic_v1.0_241009.pdf
│   └── home_assistant_voice_preview_edition_datasheet_v1_1.pdf
└── README-airplay-upstream.md          # preserved airplay-esp32 README
```

The `esphome/` directory is pre-pivot voice-assistant code; it is orphaned. The last good state of that work is tagged `v0.1-voice` and lives on the history for recovery. It will be deleted once Phase 0 lands.

## Hardware

- **Home Assistant Voice Preview Edition** (NC-VK-9727), ESP32-S3R8 + XMOS XU316 + TLV320AIC3204 + 12× WS2812B ring + rotary encoder + 3.5 mm jack with detect + TPA6211A internal speaker amp.
- **USB-C** for power + flash + serial (native CDC).
- **A 3.5 mm stereo cable** to the Denon AUX input.

See [docs/home-assistant-voice-pe-dev/home-assistant-voice.yaml](docs/home-assistant-voice-pe-dev/home-assistant-voice.yaml) for the authoritative pin map. Locally: DAC on I2C GPIO5/6 @ 0x18; I2S out GPIO7/8/10 (master); jack-detect GPIO17 (200 ms debounce); internal amp enable GPIO47; encoder GPIO16/18; LED ring GPIO21; hardware mute GPIO3.

## Build quickstart (macOS)

Prerequisites (one-time):

```bash
# 1. ESP-IDF v5.4.1 at ~/esp/esp-idf
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git

# 2. Homebrew tools
brew install cmake ninja python@3.13

# 3. Python shim — ESP-IDF's dep check hits a Python-3.9 bug on macOS system python.
#    We redirect ESP-IDF to homebrew's python3.13 via a persistent shim.
mkdir -p ~/esp/denair-python-shim
ln -sf /opt/homebrew/bin/python3.13 ~/esp/denair-python-shim/python3

# 4. Install ESP-IDF tools (via the shim so the venv is py3.13)
env -i HOME=$HOME PATH=~/esp/denair-python-shim:/opt/homebrew/bin:/usr/bin:/bin \
  ~/esp/esp-idf/install.sh esp32s3
```

Clone the repo, drop in WiFi credentials, build:

```bash
cp wifi_config.h.example wifi_config.h       # fill in WIFI_SSID_1 + WIFI_PASSWORD_1
tools/denair-build.sh reconfigure            # first run / after sdkconfig change
tools/denair-build.sh build
tools/denair-build.sh flash                  # needs the board plugged in
tools/denair-build.sh monitor
```

`DENAIR_TARGET=esp32s3 tools/denair-build.sh build` builds the upstream airplay-esp32 baseline for smoke-test comparison.

## Phase 0 gate

Pass/fail for the viability PoC is **≥100 KB free internal heap sustained for 10 min with an active AirPlay session**. Harness:

```bash
# in one terminal
tools/denair-build.sh monitor

# in another
python3 tools/heap_probe.py --port /dev/cu.usbmodem* --tag idle     --duration 600 --out phase0_idle.csv
python3 tools/heap_probe.py --port /dev/cu.usbmodem* --tag playing  --duration 600 --out phase0_playing.csv
python3 tools/heap_probe.py --port /dev/cu.usbmodem* --tag pause_cy --duration 600 --out phase0_pause.csv
```

Outcomes:
- **GREEN** — heap stable ≥100 KB: proceed to Phase 1 (stable audio, encoder volume, jack auto-switch, basic VU meter).
- **YELLOW** — marginal: trim upstream modules (display already off; buffer sizes next) and re-measure.
- **RED** — heap insufficient: fall back to AirPlay 1 via ESP-ADF `esp_airplay`, or offload AirPlay to an RPi Zero 2W feeding I2S.

See `~/.claude/plans/let-s-change-the-scope-floofy-cat.md` for the full plan and phase roadmap.

## Configuration

WiFi credentials are **compiled in** via `wifi_config.h` per the PRD. The file is gitignored; copy `wifi_config.h.example` and fill in. On each boot the board layer seeds NVS with the compile-time values, so re-flashing with new credentials is the source of truth.

If `wifi_config.h` is absent, the firmware falls through to airplay-esp32's captive-portal setup flow (connect to `ESP32-AirPlay-Setup`, configure via browser). Useful for first-time hardware probes; not the supported DenAir flow.

The advertised mDNS name defaults to `DenAir`; override via menuconfig or set at runtime via the upstream web UI.

## Licensing

This project is a fork of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32), which is **non-commercial use only**. DenAir inherits that license. Suitable for personal hardware projects; do not redistribute or sell.

Attribution:
- AirPlay receiver, board abstraction, OLED display layer, DAC drivers for TAS57xx/TAS58xx: [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) (MIT-style non-commercial).
- AIC3204 register sequence: ported from [ESPHome's aic3204 component](https://github.com/esphome/esphome/tree/dev/esphome/components/aic3204) (MIT/Apache-2.0 dual-license).
- AirPlay protocol work stands on the shoulders of [shairport-sync](https://github.com/mikebrady/shairport-sync) and [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver).
