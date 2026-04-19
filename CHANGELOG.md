# Changelog

All notable changes to this project will be documented in this file. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses a loose semver: the minor number corresponds to a
phase from the PRD (0 = viability, 1 = stable audio + controls, 2 = Phase 2
features, etc.).

Tags:
- `v0.2.0-phase1` — Phase 1 complete
- `v0.1.0-phase0` — Phase 0 viability gate GREEN

## [Unreleased]

### Added
- `CHANGELOG.md` (this file).

## [0.2.0-phase1] — 2026-04-19

First build that runs end-to-end on the Home Assistant Voice PE with all
Phase 1 PRD features except jack-detect auto-switch. Audio streams from
an iPhone / Mac via AirPlay 2 out of the 3.5 mm jack; the user has
physical volume, play-control, and mute; the 12-LED ring reacts to the
music and follows the album artwork. See
[`docs/phase1-report.md`](docs/phase1-report.md).

### Added
- **`components/ha_airplay_artwork`** — JPEG artwork decoder that extracts a
  dominant hue and pushes it to the LED ring as the PLAYING-mode base
  colour. Uses tjpgd from the ESP32-S3 ROM (zero flash cost). Decode
  runs in a queued background task on core 0; ~15 ms per 512×512 image
  at 1/8 scale. Falls back to time-based hue rotation when the image is
  near-grey.
- **`components/ha_airplay_ui`** — rotary encoder on GPIO16/18, center
  button on GPIO0 with short/double/triple/long-press detection, mute
  slide on GPIO3 repurposed as LED-ring power on/off. Registers an
  RTSP-events callback so LED states follow the AirPlay session.
- **`components/ha_airplay_leds`** — 12-LED WS2812B render engine on GPIO21
  with six-state priority machine (muted / volume / connection-in /
  connection-out / playing / idle). `audio_tap.c` feeds the renderer a
  bass-band RMS and beat detector driven from `playback_task`.
- **LED power-rail control** on GPIO45 — the WS2812B VCC is switched,
  not continuous. The mute slide drives power; ring is physically dark
  when the slide is in the "on" position (LED-off).
- **Dominant-hue feedback** across the ring with a vivid-only gate
  (saturation > 0.18, value > 0.12) to avoid muddy near-grey renders.
- **Two-SSID WiFi fallback** supervisor in the board layer — swaps to
  the alternate SSID after ~15 s of scan failures, then back if needed.
- **Compile-time WiFi credentials** via `wifi_config.h` (git-ignored).
  Board init seeds NVS on every boot so re-flashing is the source of
  truth.
- **Per-module and general documentation**: `README.md` rewritten as a
  landing page with a doc index; new `docs/{architecture,hardware,build,
  led-ux,phase1-report}.md`; per-component READMEs under `components/`.

### Changed
- Button short-press → pause/play (local), long-press → persistent DAC
  mute. Double/triple click wired to DACP next/prev (harmless no-ops on
  modern iOS but future-compatible if MFi support ever lands).
- Encoder turn clears hard-mute — matches natural "turn-to-unmute" UX.
- Upstream's RTSP artwork handler now forwards the JPEG bytes to our
  artwork decoder in addition to its existing log.

### Fixed
- **Console output on HA Voice PE**: USB CDC as primary console
  produced no log output despite the Kconfig choice landing. Reverted
  to UART primary + USB\_SERIAL\_JTAG secondary, which reliably
  produces logs over the native USB-C port.
- **Kconfig choice propagation**: `idf.py set-target` was being run
  before `tools/ha-airplay-build.sh` layered our
  `sdkconfig.defaults.ha_voice_pe`, so board / DAC / console choices
  fell back to upstream defaults. The build wrapper now exports
  `SDKCONFIG_DEFAULTS` into the environment before any `idf.py`
  invocation.
- **Link order trick**: Home Assistant AirPlay component init calls were moved from
  `components/boards/ha_voice_pe/board.c` into `main/main.c` because
  ESP-IDF orders the ha_airplay_* libraries before `libboards.a` in the
  component graph and the one-pass linker fails to resolve references
  pointing "back". Putting them in `libmain.a` (linked last) fixes it.
- **LED ring dark on early bring-up**: added a 15 ms LDO-settle wait
  after turning GPIO45 HIGH before the first pixel refresh. Without it
  the first frame rendered corrupted bytes.
- **Port auto-discovery** in `tools/ha-airplay-build.sh` — filters out
  macOS's own `/dev/cu.debug-console` which `idf.py`'s default
  discovery sometimes picks and fails on.
- **Mute slide polarity** — flipped to LOW = LEDs on after bench check.

### Removed / Reverted
- **`CONFIG_AIRPLAY_FORCE_V1=y`** experiment. Modern iOS no longer
  sends DACP-ID headers to non-MFi AirPlay devices even in AirPlay 1
  mode — upstream's own source comment confirms this. AP1 gave us zero
  benefit (no encoder → iPhone slider sync) at the cost of HomeKit
  compatibility, so the flag is unset.

### Infrastructure
- `tools/ha-airplay-build.sh`: auto-discovers `/dev/cu.usbmodem*`, exports
  `SDKCONFIG_DEFAULTS` early, sets up the Python 3.13 shim on PATH,
  prepends `/opt/homebrew/bin` so Homebrew `cmake` + `ninja` resolve.
  Subcommands: `reconfigure`, `build`, `flash`, `monitor`, `clean`,
  `menuconfig`.

## [0.1.0-phase0] — 2026-04-19

Viability gate — AirPlay 2 stack proven to fit on ESP32-S3 alongside
WiFi, mDNS, and Home Assistant AirPlay board support with ≥100 KB free internal heap
under active streaming. Measured **168 KB free internal, 5.4 MB PSRAM
free** during an active ALAC session. See
[`docs/phase0-report.md`](docs/phase0-report.md).

### Added
- Upstream [`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32)
  merged wholesale at repo root as the base firmware (RTSP, HAP,
  audio pipeline, PTP, mDNS, web server, OTA). An `airplay-upstream`
  git remote tracks the source for future selective merges.
- `components/boards/ha_voice_pe` — board init that brings up I²C on
  GPIO5/6, drives the TPA6211A amp-enable on GPIO47, registers the
  AIC3204 DAC. Pin map baked in per the upstream HA Voice PE yaml.
- `components/dac_tlv320aic3204` — full ESPHome-parity register
  sequence port for the codec: PLL-free BCLK clocking, I²S 32-bit
  interface, both HP and LO paths always routed, 2.5 s HP soft-step,
  volume via page-0 regs 65/66, `dac_ops_t` vtable.
- `sdkconfig.defaults.ha_voice_pe` — Home Assistant AirPlay's Kconfig overlay (board
  selection, DAC selection, PSRAM / flash settings, console, console
  mode).
- `tools/heap_probe.py` — USB-CDC log tailer that emits a CSV of free
  internal / SPIRAM heap per sample. Gate evidence harness.
- `tools/ha-airplay-build.sh` first pass — idf.py wrapper handling the
  Python-venv shim and SDKCONFIG_DEFAULTS plumbing.
- `docs/phase0-report.md` — verdict, gate table, evidence, known
  follow-ups.
- `README.md` — initial Home Assistant AirPlay landing page.

### Infrastructure
- ESP-IDF v5.4.1 toolchain nailed down on macOS 15.4:
  - `~/esp/esp-idf` at the v5.4.1 tag with shallow submodules.
  - `~/esp/ha-airplay-python-shim/python3` symlink to Homebrew's
    python3.13, to work around a Python-3.9
    `importlib.metadata`-normalisation bug that broke ESP-IDF's
    dependency check.
  - Homebrew `cmake` and `ninja`.

### Known limitations at gate
- Audio out of the 3.5 mm jack verified on bench but not yet soak-
  tested.
- End-to-end latency not measured numerically (stream plays without
  observable lag but no scope/loopback data).
- Jack-detect (GPIO17) wired but not read — internal amp permanently
  on.
- No encoder volume yet, no LED engine yet.

[Unreleased]: https://github.com/jorgeorejas/home-assistant-airplay/compare/v0.2.0-phase1...HEAD
[0.2.0-phase1]: https://github.com/jorgeorejas/home-assistant-airplay/compare/v0.1.0-phase0...v0.2.0-phase1
[0.1.0-phase0]: https://github.com/jorgeorejas/home-assistant-airplay/releases/tag/v0.1.0-phase0
