# Project review — consolidated punch list

**Date:** 2026-05-01
**Branch:** `phase0-raop` at the time the review ran (`e66be29`); shipped in `main` at `v0.5.0`.
**Inputs:** `architecture.md`, `performance.md`, `reliability.md`, `ux.md`, `security.md` (this directory).

Each review produced 6–10 findings; this file ranks them by **payoff / effort** across the whole project. Severity language preserved from the source reports. The detail and `path:line` references live in the per-area files; this is the orchestration view.

## Status legend

- ✅ **shipped** — landed in `v0.5.0` (Sprints A–D).
- 🛠 **partial** — addressed in part; remainder noted.
- 📅 **open** — not started; lives as backlog.

Items below are annotated with their post-`v0.5.0` status.

---

## Tier 1 — fix soon (cheap fixes for biting bugs) — ✅ all shipped

| # | Status | Finding | Source | Effort |
|---|:--:|---|---|---|
| 1 | ✅ | **OTA never calls `esp_ota_mark_app_valid_cancel_rollback`.** Either every reboot rolls back, or a bad image bricks the device. Add the call once we observe `s_airplay_started + wifi connected ≥ 60 s`. | reliability P0 | S |
| 2 | ✅ | **Audio dropouts root cause is the anchor-late detector, not buffer depth.** `MAX_CONSECUTIVE_LATE = 3` (~24 ms) trips on routine 2.4 GHz jitter. Bump to 12-16 (≥100 ms). | reliability P1 | S |
| 3 | ✅ | **Init panics on transient errors.** `ESP_ERROR_CHECK(rtsp_server_start)` runs on every network-up edge — a transient `ESP_FAIL` on hour 73 reboots the box. Convert the post-init runtime calls to soft-fail-and-retry. | reliability P0 | M |
| 4 | ✅ | **Mid-stream `MALLOC_CAP_INTERNAL` in artwork decoder** — 3.2 KB churned per JPEG push. Allocate once at init. | performance HIGH | S |
| 5 | ✅ | **`parse_dmap_metadata` recurses with no depth limit** — crafted RTSP body crashes the box. One-line depth guard. | security MED | S |
| 6 | ✅ | **Doc drift around the slide switch.** Five files still described `set_power` API and "rail gates the slide". | architecture #5/#6, ux L10 | S |
| 7 | ✅ | **Unify the three `…_BANDS = 15` constants** onto `AUDIO_EQ_BANDS`. | architecture #3 | S |

## Tier 2 — high-payoff improvements (medium effort)

| # | Status | Finding | Source | Effort |
|---|:--:|---|---|---|
| 8 | ✅ | **Artwork on the homepage** + `/api/artwork.jpg` with content ETag. | ux L2 | S |
| 9 | ✅ | **Push-driven now-playing** — `/ws/now_playing` WebSocket broadcasts on every RTSP event. Dashboard uses it with polling fallback. v0.8.0. | ux L1 | S |
| 10 | 📅 | **Realtime RX stacks force-pinned to internal RAM**. Hypothesis: 10–20 KB internal heap recoverable. Needs benchmarking. | performance HIGH | M |
| 11 | ✅ | **Phase 2: jack-detect on GPIO17 → GPIO47 amp toggle** + 5 s output-destination LED overlay. | ux table, architecture #9 | M |
| 12 | ✅ | **Chime path** — −6 dB attenuation + clean stop on real audio frames (no mid-arpeggio resume). `apply_volume` is a no-op here because AIC3204 hardware volume applies on top. | ux L5 | S |
| 13 | ✅ | **`now_playing_get` leaves `*out` untouched on timeout** instead of zeroing. | reliability P2 | S |
| 14 | ✅ | **Friendly-name UI** in dashboard + factory-seed-once for credentials. | ux L4 | M |
| 15 | 📅 | **Boot-time 2.5 s DAC ramp blocks `app_main`.** Move codec init to a parallel one-shot task. | performance MEDIUM | M |
| 16 | 📅 | **Link-order workaround** still in place (the architecture #2 finding). The CMake one-line fix wasn't applied; init calls remain in `main.c`. | architecture #2 | S |
| 17 | 📅 | **OTA token gate** — deferred. Security review notes this as accepted risk for residential threat model; revisit if device ever moves to less-trusted network. | security HIGH | S |
| 18 | 📅 | **Rotate the WiFi PSK** and redact `wifi_config.h`. The file is `.gitignore`d, but the leak vector flagged in the security review (file → LLM paste / build log) still exists. | security HIGH | S |

## Tier 3 — clean-up and delight (mostly open)

| # | Status | Finding | Source | Effort |
|---|:--:|---|---|---|
| 19 | ✅ | **`main/hap/` namespace is HomeKit-shaped but holds AirPlay 2 transient pairing.** Renamed to `main/airplay_pair/` (public header `airplay_pair.h`) in v0.6.0. | architecture #1 | M |
| 20 | ⏸ | **Dead upstream code** in Voice PE binary (`main/led.c`, `main/buttons.c`). Decided to keep — call sites in `main.c`/`audio_output.c`/`a2dp_sink.c` reference them and the no-op cost is sub-µs / frame, ~5 KB flash. Documented in `main/CMakeLists.txt`. | architecture #7/#8 | S |
| 21 | ✅ | **DMA: 8×512** shipped in v0.6.0 — same 93 ms buffer, half the ISR rate. | performance HIGH | S |
| 22 | ✅ | **EQ commit feedback loop** — 0.5 dB slider step, active-preset highlight, "Saving…" → "Applied" status pill that requires a GET round-trip to confirm. v0.8.0. | ux L3 | M |
| 23 | ✅ | **Volume curve non-linear** — 3 dB below -18 dB, 1.5 dB above. v0.6.0. | ux L8 | S |
| 24 | ✅ | **Slide-switch state on dashboard** — `decorative_leds` field in `/api/system/info`, surfaced as a Status row. v0.7.0. | ux L6 | S |
| 25 | ✅ | **Track-change ring sweep** — 1.5 s rotating shimmer in artwork hue on `RTSP_EVENT_METADATA` (with title de-bounce). v0.7.0. | ux delight | S |
| 26 | ✅ | **Sleep timer** card on dashboard (15 / 30 / 60 / 120 min + Cancel) with 60 s fade-out. v0.7.0. | ux delight | S |
| 27 | ✅ | **Don't tear down AP after STA gets IP** — APSTA stays up for the lifetime of the device. v0.6.0. | reliability P1 | S |
| 28 | ✅ | **`gpio_install_isr_service(ESP_INTR_FLAG_IRAM)`** in all four ha_airplay_ui ISR sites. v0.6.0. | reliability P2 | S |
| 29 | ✅ | **`dac_tas58xx` is now a conditional dep** gated on `CONFIG_DAC_TAS58XX`. v0.6.0. | architecture #4 | S |
| 30 | ✅ | **LED render 30 Hz** (was 50 Hz) — visually identical on 12 pixels, ~40 % fewer core-1 wakeups. v0.8.0. | performance MEDIUM | S |

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

## Execution log (post-review)

The review's suggested batching was followed, then expanded with two
follow-up sprints (E, F) covering the cleanup + delight tier.

- **Sprint A — bug-fix OTA**: ✅ shipped — items **1, 2, 3, 5, 6, 7**.
- **Sprint B — feel polish OTA**: ✅ partially shipped — items **8, 12, 13**.
- **Sprint C — Phase 2 jack-detect**: ✅ shipped — item **11** with the
  5 s output-destination LED overlay.
- **Sprint D — security tighten + name UI**: 🛠 partial — item **14**
  shipped; items **17, 18** deferred per the user's "security low
  priority" note.
- **Sprint E — architecture cleanup + reliability polish** (v0.6.0): ✅
  shipped — items **19, 21, 23, 27, 28, 29**.
- **Sprint F — UX delight** (v0.7.0): ✅ shipped — items **24, 25, 26**.
- **Sprint G — push + polish** (v0.8.0): ✅ shipped — items **9, 22, 30**.

A short HomeKit experiment ran on a `feat/homekit` branch (not part of
the review backlog) — added `espressif/esp-homekit-sdk` as a submodule
and exposed a Smart Speaker accessory. Pairing flow worked but
AirPlay's RTSP/UDP socket usage collided with HAP's HTTP server
reservation. Reverted; recommended path for HomeKit integration is HA's
HomeKit Bridge.

## Remaining backlog

After Sprints A–G, what's still open:

- **#10 PSRAM stacks experiment** — hypothesis for recovering 10–20 KB
  of internal heap; needs benchmarking + risk testing.
- **#15 boot-time DAC ramp parallelization** — would shave ~2 s off
  the ~17 s boot. Touches startup ordering.
- **#17 OTA token gate / #18 PSK rotation** — security; user-accepted
  risk for the residential threat model.
- **#20 dead upstream code (`led.c`/`buttons.c`)** — kept on purpose;
  the no-op cost is below the call-site disruption.

Phase 2 PRD remainder (separate from the review backlog):

- Spectrum LED mode (256-pt FFT via ESP-DSP, 12 log bands)
- Long-press LED mode cycle
- Persistent LED mode in NVS

26 of 30 review items shipped (24 ✅ + 2 ⏸/closed-by-decision). None
of the remaining items block anything; pick from the list as
motivation strikes.
