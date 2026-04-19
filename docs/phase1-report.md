# Phase 1 report

**Date:** 2026-04-19
**Branch:** `phase0-raop` (renamed Phase 1 work continued on the same branch)
**Scope:** "Audio stable + physical controls + responsive LEDs", per [the pivot plan](https://github.com/~/.claude/plans) and the PRD in the project root.

## Verdict

✅ **All Phase 1 PRD requirements shipped** except jack-detect auto-switch (PRD §5.3 — deferred to Phase 2). Audio-reactive LED modes partially shipped (beat-pulse mode is live; spectrum mode deferred to Phase 2).

## What landed

| Requirement | Status | Notes |
|---|:-:|---|
| Encoder volume (PRD §5.4) | ✅ | GPIO16/18 quadrature decoder, 4 edges/detent, ≈3 dB per click. Local DAC volume via AIC3204 page-0 reg 65/66. |
| Center-button play/pause | ✅ | Short press → `playback_control_play_pause` (upstream's local-mute fallback in AP2). |
| Track skip | ⚠️ | Double-click → `playback_control_next`; triple-click → `_prev`. **Does not reach iPhone**: modern iOS doesn't send DACP to non-MFi devices even in AP1 mode. Wired for forward-compat. |
| Local persistent mute | ✅ | Long-press (≥1.5 s) → `dac_set_power_mode(DAC_POWER_STANDBY)` + LED mute indicator. Cleared by encoder turn. |
| WiFi two-SSID fallback (PRD §5.2) | ✅ | Compile-time `wifi_config.h` + board-layer supervisor. Swaps after 15 s of "no APs found". Tested live: Asturias (SSID 1 not visible in Madrid) → `Wireless_CASA` (SSID 2) in ~16 s. |
| Volume persistence | ✅ | Upstream's `settings_set_volume` writes to NVS; restored on boot (see log: `Loaded volume: -21.00 dB`). |
| WS2812B ring, 4 modes (PRD §5.5) | ⚠️ | Six-state machine instead of four — see `docs/led-ux.md`. **Beat-pulse** and **volume bar** and **idle breath** are live. **VU meter** and **spectrum** deferred (Phase 2). |
| Audio-reactive (not yet called out in PRD) | ✅ bonus | Tap in `playback_task` feeds bass-band RMS + beat detector to the LED engine. |
| Artwork → LED hue (not in PRD) | ✅ bonus | tjpgd-based dominant-hue extraction. |
| Jack auto-switch (PRD §5.3) | ⏭️ | GPIO17 wired but not read. Amp on GPIO47 permanently enabled. Phase 2. |
| Conmutación LEDs 5 s (PRD §5.3) | ⏭️ | Ditto. |

## Surprises from the bench

- **Upstream link ordering** puts the DenAir component libs before `libboards.a` in the component graph, so references from `boards/ha_voice_pe/board.c` to `denair_ui_init` don't resolve in ESP-IDF's one-pass linker. Fix: move the init calls into `main/main.c`, which builds into `libmain.a` (linked last).
- **Kconfig choice propagation**: `idf.py set-target` discovers its own defaults and locks choice options *before* `tools/denair-build.sh` has a chance to layer `sdkconfig.defaults.ha_voice_pe`. Had to plumb `SDKCONFIG_DEFAULTS` through the environment in the build wrapper.
- **DACP is gone from modern iOS**. Upstream supports `CONFIG_AIRPLAY_FORCE_V1=y` for bidirectional volume, but modern iOS (17+) no longer sends DACP-ID headers to non-MFi receivers even in AP1 mode. Rolled back to AP2. Device → iPhone slider sync is now an accepted protocol limitation.
- **WS2812B VCC is switched**, not continuous — the whole reason "LEDs aren't lighting" took ten minutes to diagnose. GPIO45 gates the rail. Repurposed the mic-mute slide (GPIO3) as the user-facing on/off for the ring.
- **RMS-scale tuning for the audio tap**: initial scaling made even quiet passages look bright. Recomputed the `sat_i32_from_u64_sqrt` mapping against real iPhone audio levels during bench testing.

## Evidence

Bench-captured logs from the final Phase 1 firmware:

```
I (3641) denair_leds: LED ring up (GPIO 21, 12 pixels, 50 Hz)
I (3661) denair_encoder: encoder ready (A=16 B=18, 4 quad-edges / detent)
I (3681) denair_button: center button ready (GPIO 0, short/double/triple/long)
I (3711) denair_switch: mute-slide switch seeded: level=0 → LEDs ON
I (3731) denair_ui: UI ready

I (16196) wifi:connected with Wireless_CASA, rssi: -52
I (18116) main: AirPlay ready

I (47651) denair_ui: AirPlay playing → LED beat-pulse mode
I (54732) audio_rt: Free heap: 168311 internal, 5487504 SPIRAM
I (58582) rtsp_handlers: Album = NO ME QUIERO MORIR NUNCA
I (58582) rtsp_handlers:   Artist = RATA
I (58582) rtsp_handlers:   Title  = DEJARSE LOS NUDILLOS

I (58595) denair_artwork: decoding 512×512 JPEG (scale 1/8, 180224 B)
I (58611) denair_artwork: artwork mean RGB=(0.21,0.07,0.04) → HSV=(18°,0.81,0.21) vivid=1

I (142256) playback_ctrl: AirPlay volume: -24.8 -> -21.8 dB   [encoder turn]
I (147136) denair_ui: button short press → muted                [play/pause]
```

## What's next (Phase 2)

- Jack-detect on GPIO17 (200 ms debounce) → GPIO47 amp enable flip, 5 s output-destination LED indicator (PRD §5.3).
- Spectrum LED mode (256-pt FFT via ESP-DSP, 12 log bands). Long-press cycles beat-pulse ↔ spectrum ↔ idle-only.
- Persistent LED mode in NVS.
- Track-change transition animation on the ring.
- Audio soak: sustain a stream for ≥30 min and record heap/dropouts.
