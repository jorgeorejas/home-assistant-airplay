# Changelog

All notable changes to this project will be documented in this file. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses a loose semver: the minor number corresponds to a
phase from the PRD (0 = viability, 1 = stable audio + controls, 2 = Phase 2
features, etc.).

Tags:
- `v0.8.0` — Sprint G (push-driven now-playing, EQ feedback polish, LED 30 Hz)
- `v0.7.0` — Sprint F (UX delight: sleep timer, track-change ring sweep, slide state)
- `v0.6.0` — Sprint E (architecture cleanup: hap → airplay_pair, conditional deps, ISR IRAM, AP+STA forever, non-linear volume, DMA refinement)
- `v0.5.0` — Sprint A–D follow-ups + Phase 2 jack-detect
- `v0.2.0-phase1` — Phase 1 complete
- `v0.1.0-phase0` — Phase 0 viability gate GREEN

## [Unreleased]

_(no entries)_

## [0.8.0] — 2026-05-01

Push-driven now-playing + EQ commit polish + LED render frequency
tweak. Three small review items rolled together.

### Added
- **`/ws/now_playing` WebSocket** (`main/network/now_playing_ws.{c,h}`)
  — broadcasts a JSON snapshot to all connected clients on every
  RTSP event (connect, playing, paused, disconnected, metadata).
  Connect-time send is the latest snapshot so the page renders
  immediately. Up to 3 concurrent clients (mirrors `log_stream.c`).
  Visible play-tap → UI-update latency drops from ~1 s median to
  &lt;50 ms.

### Changed
- Dashboard `/` swaps polling `setInterval(refreshNp, 2000)` for the
  WebSocket above. Exponential-backoff reconnect with a polling
  fallback that activates after the second failed attempt — keeps
  the page working against older firmware where `/ws/now_playing`
  isn't registered.
- EQ slider step 1 dB → 0.5 dB.
- EQ commit feedback: an "active preset" border highlights the matching
  preset button when current gains exactly match (within 0.05 dB). A
  status pill above the EQ flips "Saving…" (orange) → "&lt;preset&gt; preset"
  / "Custom" (green) only after a follow-up GET round-trips
  successfully.
- LED render frequency 50 Hz → 30 Hz. Visually identical on a 12-pixel
  ring at our brightness range; ~40 % fewer wakeups on core 1.
- `config.max_uri_handlers` 20 → 32 in `web_server_start` so the
  WebSocket route fits alongside captive portal + EQ + artwork +
  sleep_timer + ws/logs.

## [0.7.0] — 2026-05-01

UX delight pass — three review backlog items shipped on top of 0.6.0.

### Added
- **Sleep timer.** New `main/sleep_timer.c` with one-shot
  `esp_timer`-based scheduler. POST `/api/sleep_timer {"minutes":N}`
  arms it; the dashboard adds a card with 15 / 30 / 60 / 120 minute
  presets and Cancel. Last 60 s smoothly fade digital volume from
  current to -30 dB; at T=0 the codec drops to STANDBY. Pre-timer
  volume is captured and persisted so the next encoder turn / unmute
  restores it. Cancel-mid-fade restores volume immediately.
- **Track-change ring sweep.** New
  `ha_airplay_leds_show_track_change()` — 1.5 s rotating shimmer in
  the artwork hue (two pixels lead, dim base of the same hue) on every
  new RTSP_EVENT_METADATA title. De-bounced in `ha_airplay_ui/ui.c` so
  progress-update metadata pings don't retrigger. Decorative-only —
  gated off when the slide is off.
- **`decorative_leds`** field in `/api/system/info` mirrors the slide
  state. Dashboard surfaces it as a Status row that reads `on` or
  `off (slide off)`.

### Changed
- `version.txt` → 0.7.0.

## [0.6.0] — 2026-05-01

Architecture and reliability cleanup pass — Sprint E from
`docs/review/SUMMARY.md`. No user-visible feature changes; two
behavior refinements you'll feel (smoother volume curve, halved I²S
ISR cadence).

### Changed
- **`main/hap/` → `main/airplay_pair/`** with public header renamed
  `hap.h` → `airplay_pair.h` (architecture review #1). The directory
  contained AirPlay 2 transient-pairing crypto, never HomeKit;
  rename eliminates the misleading namespace. Internal `hap_*` symbols
  kept (no cross-component conflict). Cross-component callers in
  `main.c`, `network/mdns_airplay.c`, `rtsp_conn.h`, `rtsp_handlers.c`
  updated to `#include "airplay_pair.h"`.
- **Non-linear volume curve.** 3 dB/click below the -18 dB knee,
  1.5 dB above. From a comfortable -12 dB a single click is now ~20 %
  SPL instead of ~40 %. UX review L8.
- **I²S DMA: 16×256 → 8×512.** Same 93 ms buffer, half the
  DMA-completion ISR rate. Performance review HIGH.
- **WiFi: keep AP+STA forever.** No longer tear down the AP netif on
  `IP_EVENT_STA_GOT_IP`. Cost: ~20 KB lwIP state and one extra SSID
  broadcast. Win: captive portal stays reachable if creds go stale.
  Reliability review P1.
- **`gpio_install_isr_service(ESP_INTR_FLAG_IRAM)`** in all four
  `ha_airplay_ui` ISR sites (button, encoder, led_switch,
  jack_detect). Encoder ticks no longer lost during NVS commits or
  OTA flash writes. Reliability review P2.
- **`dac_tas58xx` is now a conditional dep** of `main` gated on
  `CONFIG_DAC_TAS58XX`. Voice PE no longer pulls in a chip family it
  never uses. Architecture review #4.
- `version.txt` → 0.6.0.

### Notes
- Architecture review #7/#8 (drop dead `led.c`/`buttons.c` on Voice PE)
  was deliberately not applied. Call sites in `main.c`/`audio_output.c`/
  `a2dp_sink.c` reference the symbols, so the no-op pattern (every
  CONFIG_*_GPIO=-1 → early return) is the cheaper option than gating
  out + stubbing all the call sites. Documented in `main/CMakeLists.txt`.

## [0.5.0] — 2026-05-01

Web dashboard, software EQ, jack-detect (Phase 2 first slice), and a
project-wide reliability/UX pass driven by a multi-agent review. Final
state of `main` after PR #1 plus the slide-switch behavior change.

### Added — features
- **Embedded web dashboard** at `http://<slug>.local/` — single-page UI
  with status, now-playing card (title / artist / album / album art),
  15-band EQ + 5 presets, device-name editor, restart. HTML lives in
  `main/network/index.html`, embedded into the binary via `EMBED_FILES`
  (no SPIFFS dependency).
- **Live log viewer** at `/logs` — WebSocket-streamed device logs with
  ANSI-strip, level coloring (I/W/E/D), substring filter, pause,
  autoscroll, exponential reconnect.
- **Software 15-band parametric EQ** in the audio task. 15 cascaded
  peaking biquads (DF2T) on the LX7 FPU, ~620 µs per 8 ms block.
  Coefficients are double-buffered with an atomic-int swap so the web
  task never blocks the hot path. Gains persist to NVS; restored on
  boot. Five preset curves (Flat / Loudness / Vocal / Bass+ / Treble+).
  Implemented in `main/audio/audio_eq.{c,h}`.
- **Now-playing endpoint** (`/api/now_playing`) — RTSP-derived metadata
  cached in `main/now_playing.c`. Subscribes to the `rtsp_events` bus,
  exposes title / artist / album / genre / duration / position / state /
  artwork_etag.
- **Album-art serving** (`/api/artwork.jpg`) — the `ha_airplay_artwork`
  component now retains the most recent JPEG in PSRAM after decoding it
  for hue extraction. Served with FNV-1a content-derived ETag and
  conditional-GET (`If-None-Match` → 304). Decode pool is allocated
  once at init instead of per track, removing a recurrent ~3 KB
  internal-heap churn.
- **MagSafe-style connection chime** — short ascending bell arpeggio
  (vendored macOS PowerChime PCM, ~156 KB embedded) plays on
  `RTSP_EVENT_CLIENT_CONNECTED`. Attenuated −6 dB so it doesn't peak at
  full digital scale; cuts cleanly when real audio frames arrive.
- **Phase 2: jack-detect** on GPIO17 (200 ms debounced). Drives GPIO47
  LOW when the 3.5 mm plug is detected to mute the internal amp; a 5 s
  cyan/amber LED overlay confirms the new output destination. Renders
  in the utility-overlay slot so it shows even with the slide off.
  New module `components/ha_airplay_ui/jack_detect.c`.
- **Friendly mDNS hostname slug** — UTF-8 friendly name (e.g.
  `Altavoces Salón`) auto-folds to RFC-1123 ASCII (`altavoces-salon`)
  for `<slug>.local`. Diacritics are folded inline (no iconv dep).
- **Friendly-name editor** in the dashboard — POSTs to `/api/device/name`,
  persists to NVS, hostname slug refreshes on next boot. Live preview
  of the resulting `<slug>.local` URL.
- **OTA validity marking** — once the device sees 60 s of healthy
  network with AirPlay started, `network_monitor_task` calls
  `esp_ota_mark_app_valid_cancel_rollback()`. Without this, depending
  on `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a bad image either rolled
  back every boot or latched permanently.

### Added — operational / docs
- `docs/dashboard.md` — full HTTP / WebSocket API reference + curl recipes.
- `docs/review/` — five multi-agent review reports (architecture,
  performance, reliability, UX, security) plus `SUMMARY.md` punch list
  that drove this release's Sprint A–D work.

### Changed
- **Slide switch (GPIO3) gates decorative renders only.** The WS2812B
  VCC rail (GPIO45) is held HIGH for the device's lifetime; the slide
  toggles a software flag that suppresses only PLAYING + IDLE. Mute,
  volume bar, connection sweeps, and the new output-change overlay
  render unconditionally so essential feedback is always visible.
- **Factory-seed-once for credentials.** `iot_board_init` now seeds
  WiFi creds and device name from `wifi_config.h` only when NVS is
  empty. Runtime renames or credential changes via the captive portal
  / dashboard now persist across reboots; were being clobbered every
  boot before.
- **I²S DMA depth bumped** 8 → 16 descriptors (~46 → 93 ms buffered).
  Initially shipped to absorb WiFi packet jitter that produced
  user-reported sub-second dropouts.
- **Anchor-late detection** widened from 3 frames (~24 ms) to 16
  frames (~128 ms) in `audio_timing.c`. Routine 2.4 GHz WiFi jitter
  was tripping the bulk-flush threshold; this is the actual
  root-cause fix for the dropouts.
- **`rtsp_server_start` is now soft-fail** in `start_airplay_services`
  — was `ESP_ERROR_CHECK`, which would reboot-loop the box on a
  transient socket-exhaustion failure. Retries on the next
  network-monitor tick.
- **`parse_dmap_metadata` recursion is depth-bounded** (≤8). Hostile
  RTSP `SET_PARAMETER` body with deeply nested `mlit`/`cmst`/`mdst`
  containers can no longer overflow the RTSP task stack.
- **`now_playing_get` leaves `*out` untouched on mutex timeout**
  instead of zeroing — no more dashboard flicker to "nothing playing"
  during heavy RTSP traffic.
- **Three parallel `…_BANDS = 15` constants** collapsed onto a single
  canonical `AUDIO_EQ_BANDS`, with `SETTINGS_EQ_BANDS` and
  `TAS58XX_EQ_BANDS` kept as aliases.
- **Doc sweep** — `docs/hardware.md`, `docs/led-ux.md`,
  `docs/architecture.md`, `README.md`, and the relevant component
  READMEs all updated to reflect the slide-switch behavior change and
  the new feature surface.

### Notes — HomeKit experiment (reverted)
- A short-lived `feat/homekit` branch experimented with adding
  `espressif/esp-homekit-sdk` to expose the speaker as a real
  HomeKit accessory in iOS Home / macOS Home. The accessory
  pairing flow worked (Smart Speaker service, UUID 0x228, with
  Volume / Mute / TargetMediaState / CurrentMediaState), but
  AirPlay's RTSP socket usage collided with the HAP HTTP
  server's reservation: even at `CONFIG_LWIP_MAX_SOCKETS=24`
  with the HAP socket budget cut to 4, AirPlay started failing
  UDP socket allocation mid-stream. The branch was reverted;
  the recommended path forward is Home Assistant's HomeKit
  Bridge integration (zero firmware changes, ~30 min of HA
  config). The architecture review's `main/hap/ → main/airplay_pair/`
  rename was reverted along with the branch.

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
