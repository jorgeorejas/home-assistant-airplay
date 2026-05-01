# Project review — consolidated punch list

**Date:** 2026-05-01
**Branch:** `phase0-raop` (`e66be29`)
**Inputs:** `architecture.md`, `performance.md`, `reliability.md`, `ux.md`, `security.md` (this directory).

Each review produced 6–10 findings; this file ranks them by **payoff / effort** across the whole project. Severity language preserved from the source reports. The detail and `path:line` references live in the per-area files; this is the orchestration view.

---

## Tier 1 — fix soon (cheap fixes for biting bugs)

| # | Finding | Source | Effort |
|---|---|---|---|
| 1 | **OTA never calls `esp_ota_mark_app_valid_cancel_rollback`.** Either every reboot rolls back, or a bad image bricks the device. Add the call once we observe `s_airplay_started + wifi connected ≥ 60 s`. | reliability P0 | S |
| 2 | **Audio dropouts root cause is the anchor-late detector, not buffer depth.** `MAX_CONSECUTIVE_LATE = 3` (~24 ms) trips on routine 2.4 GHz jitter. Bump to 12-16 (≥100 ms). The DMA bump we just shipped wasn't the right knob. | reliability P1 | S |
| 3 | **Init panics on transient errors.** `ESP_ERROR_CHECK(rtsp_server_start)` runs on every network-up edge — a transient `ESP_FAIL` on hour 73 reboots the box. Convert the post-init runtime calls to soft-fail-and-retry. | reliability P0 | M |
| 4 | **Mid-stream `MALLOC_CAP_INTERNAL` in artwork decoder** — 3.2 KB churned per JPEG push, fragmenting the smallest free internal-heap block when it matters most. Allocate once at init or move to PSRAM. | performance HIGH | S |
| 5 | **`parse_dmap_metadata` recurses with no depth limit** — crafted RTSP body crashes the box. One-line `if (depth > 8) return;`. | security MED | S |
| 6 | **Doc drift around the slide switch.** `docs/hardware.md`, `docs/led-ux.md`, `README.md`, `components/ha_airplay_leds/README.md`, `docs/architecture.md` still describe the deleted `set_power` API and "rail gates the slide." | architecture #5/#6, ux L10 | S |
| 7 | **Unify the three `…_BANDS = 15` constants.** `AUDIO_EQ_BANDS`, `SETTINGS_EQ_BANDS`, `TAS58XX_EQ_BANDS` must stay equal by coincidence. Drop the TAS58XX include in `eq_events.h`. | architecture #3 | S |

## Tier 2 — high-payoff improvements (medium effort)

| # | Finding | Source | Effort |
|---|---|---|---|
| 8 | **Artwork on the homepage.** JPEG already decoded in PSRAM for hue extraction; expose `/api/artwork.jpg` + 96×96 thumbnail in the now-playing card. Single biggest "iPhone-y" delight win available. | ux L2 | S |
| 9 | **Push-driven now-playing.** Replace 2 s polling with `/ws/now_playing` mirroring the `log_stream` pattern. Eliminates the visible play-tap → UI-update lag. | ux L1 | S |
| 10 | **Realtime RX stacks force-pinned to internal RAM** (`audio_stream_realtime.c:49–54`). Hypothesis: 10–20 KB of internal heap recoverable by moving to PSRAM stacks. Worth benchmarking. | performance HIGH | M |
| 11 | **Phase 2: jack-detect on GPIO17 → GPIO47 amp toggle.** Internal speaker is permanently HIGH; plugging in headphones doesn't switch outputs. More user-visible than the spectrum LED mode that's also "Phase 2". | ux table, architecture #9 | M |
| 12 | **Chime path doesn't respect digital volume** and gets cut mid-arpeggio when audio frames arrive. Apply `apply_volume` to chime PCM and add a 30 ms crossfade out. | ux L5 | S |
| 13 | **`now_playing_get` returns zeroed metadata on 20 ms timeout** → UI flickers to "nothing playing" mid-track. Leave `*out` untouched on timeout. | reliability P2 | S |
| 14 | **Friendly-name UI + boot-seed footgun.** `/api/device/name` exists but no dashboard input; `iot_board_init` re-seeds NVS from `wifi_config.h` *every* boot, overwriting any user change. Make seed a one-shot factory-default. | ux L4 | M |
| 15 | **Boot-time 2.5 s DAC ramp blocks `app_main`.** Move codec init to a parallel one-shot task; AirPlay services don't need DAC ready until first audio frame. | performance MEDIUM | M |
| 16 | **Link-order workaround based on a wrong premise.** Real fix: add `ha_airplay_*` libs to `components/boards/CMakeLists.txt` `PRIV_REQUIRES`, move three init calls back into `iot_board_init`, delete three explanatory comments. | architecture #2 | S |
| 17 | **OTA token gate** for `/api/ota/update`, `/api/system/restart`, `/api/wifi/config`. Cheapest defence against the "anyone on the LAN bricks the box" surface. | security HIGH | S |
| 18 | **Rotate the WiFi PSK** and stop committing real credentials to `wifi_config.h`. Replace with `"REPLACE_ME"` and load real values from outside the repo at flash time. | security HIGH | S |

## Tier 3 — clean-up and delight

| # | Finding | Source | Effort |
|---|---|---|---|
| 19 | **`main/hap/` namespace is HomeKit-shaped but holds AirPlay 2 transient pairing.** Rename to `airplay_pairing` — touches ~30 call sites in `rtsp_handlers.c`. | architecture #1 | M |
| 20 | **Dead upstream code in Voice PE binary**: `main/led.c` (~560 lines) and `main/buttons.c` are compiled in but every relevant GPIO is `-1`. Hot path calls `led_audio_feed` 3× per frame. Gate out of SRC_FILES under `if(NOT CONFIG_BOARD_HA_VOICE_PE)`. | architecture #7/#8 | S |
| 21 | **DMA: 8×512 strictly dominates 16×256.** Same 93 ms buffer, half the ISR rate. | performance HIGH | S |
| 22 | **EQ commit feedback loop**: 0.5 dB step (was 1), highlight active preset, real error toasts on POST failure, "applied" pill that flips green only after `/api/eq` GET round-trips. | ux L3 | M |
| 23 | **Volume curve is coarse at the loud end.** 3 dB/click below -18 dB, 1.5 dB above. | ux L8 | S |
| 24 | **Slide-switch state surfaced in dashboard + 1 s confirmation animation on the ring** when toggled. | ux L6 | S |
| 25 | **Track-change ring sweep** when `RTSP_EVENT_METADATA` brings a new title — masks the abrupt hue change. | ux delight | S |
| 26 | **Sleep timer** card on the dashboard (15/30/60 min, fade-out the last 60 s). | ux delight | S |
| 27 | **WiFi: don't tear down AP after STA gets IP.** Keep APSTA forever — captive portal stays available if credentials get stale. | reliability P1 | S |
| 28 | **`gpio_install_isr_service(0)` should pass `ESP_INTR_FLAG_IRAM`** since all three ISRs are already `IRAM_ATTR`. Encoder ticks won't be lost during NVS/OTA flash writes. | reliability P2 | S |
| 29 | **`dac_tas58xx` is an unconditional dep of `main`** even on Voice PE. Make it conditional like `bt`/`esp_eth` already are. | architecture #4 | S |
| 30 | **LED render frequency 50 → 30 Hz**, optionally pin to core 0. Visually indistinguishable on a 12-pixel ring; ~40 % fewer wakeups. | performance MEDIUM | S |

## What's healthy (don't touch)

- `audio_buffer.c` slot pool with sorted index + free stack, properly locked
- `audio_eq.c` lock-free atomic-double-buffered coefficient publish (textbook)
- `dac_ops_t` six-function vtable abstraction
- `rtsp_events` and `eq_events` observer buses (small, scoped, lock-free)
- `wifi_config.h` two-SSID fallback supervisor
- `audio_realtime_preallocate` anti-fragmentation move
- The `ha_airplay_leds` / `ha_airplay_ui` split
- `/logs` live viewer (the most polished surface — template for #9)
- Beat-pulse + artwork-hue + vivid gate (real "premium" moment)
- Long-press → DAC standby mute, cleared by encoder turn

## Suggested execution order

If you want a "next sprint" plan, batch the work as:

**Sprint A — bug-fix OTA** (~half a day): items **1, 2, 3, 5, 6, 7**. All small, all serve reliability + correctness. The dropout fix alone is a real win.

**Sprint B — feel polish OTA** (~one day): items **8, 9, 12, 13**. Artwork on the homepage + push-driven now-playing changes the whole feel of the dashboard. Chime polish is small but classy.

**Sprint C — Phase 2 jack-detect** (~one day): item **11** plus the LED "output destination" 5 s indicator from the original PRD §5.3.

**Sprint D — security tighten + name UI** (~half a day): items **14, 17, 18**. Pairs naturally because they share the dashboard.

The architecture tier 3 cleanup items are good "rainy day" work — none of them block anything else.
