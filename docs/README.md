# Documentation

Project documentation for **Home Assistant AirPlay** — an AirPlay 2 receiver firmware for the Home Assistant Voice Preview Edition.

The repo's top-level [`README.md`](../README.md) is the entry point for new readers (overview, features, quick start). This directory is reference material organized by audience: pick the path that matches what you're trying to do.

## I want to understand the system

| File | Topic |
|---|---|
| [`architecture.md`](architecture.md) | System diagram, module graph, playback-task pipeline, concurrency table, key design decisions, phase map |
| [`hardware.md`](hardware.md) | GPIO pin map, board specs, schematic and datasheet links |
| [`led-ux.md`](led-ux.md) | LED ring state machine spec — priority, render rules, beat-detect DSP, artwork → hue, slide-switch behaviour |
| [`UPSTREAM-README.md`](UPSTREAM-README.md) | Original `airplay-esp32` README, preserved verbatim for context on the upstream RTSP/HAP/audio stack we build on |

For per-module details (one component at a time), each `components/<name>/README.md` covers that component's responsibilities, public API, and design choices. Start with:

- [`components/boards/ha_voice_pe/README.md`](../components/boards/ha_voice_pe/README.md) — board layer (pin map ownership, I²C bus, WiFi seed)
- [`components/dac_tlv320aic3204/README.md`](../components/dac_tlv320aic3204/README.md) — DAC driver
- [`components/ha_airplay_leds/README.md`](../components/ha_airplay_leds/README.md) — LED engine + audio tap + beat detector
- [`components/ha_airplay_ui/README.md`](../components/ha_airplay_ui/README.md) — encoder, button, slide-switch, jack-detect
- [`components/ha_airplay_artwork/README.md`](../components/ha_airplay_artwork/README.md) — JPEG artwork → hue + retained bytes for `/api/artwork.jpg`

## I want to build, flash, or run the device

| File | Topic |
|---|---|
| [`build.md`](build.md) | Toolchain setup (ESP-IDF v5.4.1 + Python 3.13 shim), build / flash / monitor recipes, OTA over WiFi |
| [`dashboard.md`](dashboard.md) | Web UI + HTTP/WebSocket API reference for the dashboard at `http://<slug>.local/`. Every endpoint, captive-portal flow, the unauthenticated-by-design model, curl recipes |

## I want to know what shipped, when, and why

| File | Topic |
|---|---|
| [`../CHANGELOG.md`](../CHANGELOG.md) | Versioned change log with per-release Added / Changed / Notes sections |
| [`phase0-report.md`](phase0-report.md) | Phase 0 viability gate (heap, mDNS, AirPlay 2 stream) — **GREEN** |
| [`phase1-report.md`](phase1-report.md) | Phase 1 features actually shipped vs PRD list |

## I want to evaluate the code or pick something to fix

| File | Topic |
|---|---|
| [`review/SUMMARY.md`](review/SUMMARY.md) | Consolidated punch list across all five review areas, ranked by payoff/effort, annotated with shipped/open status per item |
| [`review/architecture.md`](review/architecture.md) | Module boundaries, abstraction quality, naming, dead code, CMake plumbing, doc drift |
| [`review/performance.md`](review/performance.md) | EQ hot-path budget, internal-heap pressure, DMA depth, LED render frequency, boot timeline, binary trajectory |
| [`review/reliability.md`](review/reliability.md) | OTA rollback safety, init panic paths, WiFi reconnect, mutex correctness, ISR safety, audio-dropout root cause |
| [`review/ux.md`](review/ux.md) | PRD coverage, dashboard polling vs push, encoder feel, button gestures, missing visible states, delight candidates |
| [`review/security.md`](review/security.md) | LAN-local threat model — OTA without signature, WiFi PSK at rest, HAP pairing PIN, parse_dmap recursion |

## Reference assets

The pin map and schematic in [`hardware.md`](hardware.md) cite the local PDFs:

- [`home_assistant_voice_pe_schematic_v1.0_241009.pdf`](home_assistant_voice_pe_schematic_v1.0_241009.pdf)
- [`home_assistant_voice_preview_edition_datasheet_v1_1.pdf`](home_assistant_voice_preview_edition_datasheet_v1_1.pdf)
- [`voice_preview_edition_enclosure_all_parts.stl`](voice_preview_edition_enclosure_all_parts.stl) — printable replacement enclosure
- [`home-assistant-voice-pe-dev/`](home-assistant-voice-pe-dev/) — vendored upstream ESPHome reference (authoritative for stock-firmware behaviour we don't replicate)

## Conventions

- Versioned reports (`phase0-report.md`, `phase1-report.md`) are **frozen records** — never edited after publication.
- Living docs (`architecture.md`, `hardware.md`, `led-ux.md`, `dashboard.md`, the per-component READMEs, `CHANGELOG.md`) are **kept in sync with `main`**. If you find them stale, a doc PR is appreciated.
- Review reports under [`review/`](review/) are **point-in-time audits** (2026-05-01). Findings get annotated as ✅ / 🛠 / 📅 / ⏸ in `review/SUMMARY.md` as they're addressed; the area-specific reports are left as-is so the original analysis stays auditable.
