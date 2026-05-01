# Architecture & maintainability review

**Scope:** Read-only audit of module boundaries, abstractions, naming, dead code, CMake plumbing, configuration, and doc drift across `main/`, `components/`, `tools/`, and `docs/`.
**Date:** 2026-05-01

## TL;DR
- The link-order workaround for `ha_airplay_*_init` is now anchored in three places (architecture comment, board comment, main comment) but only works because `libmain.a` happens to be the last-linked archive — easy to silently break, and the documented "PRIV_REQUIRES" reason is not the real one.
- `main/hap/` is mis-named: it implements AirPlay 2 transient pair-verify/setup, not HomeKit accessories. Anyone doing a HomeKit feature search lands in the wrong place.
- The 15-band EQ has three parallel constants (`AUDIO_EQ_BANDS`, `SETTINGS_EQ_BANDS`, `TAS58XX_EQ_BANDS`) that must stay equal by coincidence, plus a forced `dac_tas58xx` linkage on a board that has no TAS58XX.
- `docs/led-ux.md` and `components/ha_airplay_leds/README.md` still describe the deleted `ha_airplay_leds_set_power(bool)` and "slide gates the rail" model — both wrong since `e66be29`.
- The upstream legacy LED driver (`main/led.c`, ~560 lines) and the legacy `main/buttons.c` are still compiled into every Voice PE binary even though every relevant `CONFIG_*_GPIO` is `-1`.

## Findings

### [1] hap/ namespace is HomeKit-shaped but holds AirPlay 2 transient pairing
**Where:** `main/hap/hap.h:8-12`, `main/hap/hap.c:14` (NVS namespace `"airplay"`), `main/hap/hap_pair_verify.c`, `main/CMakeLists.txt:73`, `main/main.c:14,71`.
**Issue:** The directory, header, file prefix, function prefix (`hap_*`), and TAG ("hap") all read like the HomeKit Accessory Protocol — but the implementation is strictly AirPlay 2 transient pair-verify/setup over RTSP. `hap.c` even uses the NVS namespace `"airplay"`, betraying the real purpose. Anyone grepping for HomeKit accessories or HAP characteristics will assume HomeKit support exists; anyone touching pair-verify will not realise the keys here are also exposed to mDNS as the `pk=` field.
**Suggested fix:** Rename directory to `main/airplay_pairing/`, prefix functions `airplay_pair_*`, drop the dual identity from comments. `hap.h:8-12` literally says "HAP (HomeKit Accessory Protocol) implementation for AirPlay 2" — pick one. Keep the file split (verify/setup/srp/tlv8/crypto), it's well-organised internally.
**Effort:** M (mechanical rename, but touches ~30 call sites in `rtsp_handlers.c`).

### [2] Link-order workaround is structurally fragile and the stated reason is wrong
**Where:** `main/main.c:233-247`, `components/boards/ha_voice_pe/board.c:253-258`, `docs/architecture.md:106`.
**Issue:** Three different files explain the trick as "PRIV_REQUIRES ordering puts ha_airplay_* libs before libboards.a, and `libmain.a` is linked last." That is half the story: ESP-IDF's component graph topologically sorts by `REQUIRES`/`PRIV_REQUIRES`, and `libboards.a` lists `ha_airplay_artwork`/`ha_airplay_leds`/`ha_airplay_ui` nowhere in `components/boards/CMakeLists.txt:3` — so the linker has no symbols to resolve at all if the call were inside `board.c`. The real fix is one line: add `ha_airplay_leds ha_airplay_ui ha_airplay_artwork` to the `boards` PRIV_REQUIRES and the link-order pathology disappears. The current setup also means `iot_board_init` is not the single entry point its name promises — half of board bring-up lives in `app_main`.
**Suggested fix:** Add the three `ha_airplay_*` libs to `components/boards/CMakeLists.txt:3` PRIV_REQUIRES, move the three `ha_airplay_*_init` calls back into `iot_board_init` after `dac_init`, and delete the three explanatory comment blocks. If you want to keep a kill-switch, gate them on `#ifdef CONFIG_BOARD_HA_VOICE_PE`.
**Effort:** S (one CMake line + cut-paste ~15 lines from main.c into board.c).

### [3] Three parallel "15 bands" constants that must stay equal
**Where:** `main/audio/audio_eq.h:21` (`AUDIO_EQ_BANDS`), `main/settings.h:105` (`SETTINGS_EQ_BANDS`), `main/audio/eq_events.h:22` (`TAS58XX_EQ_BANDS`), `main/network/web_server.c:528-588`.
**Issue:** Three independent `#define …_BANDS 15`. `audio_eq_set_gains` takes `gains_db[AUDIO_EQ_BANDS]`, listens to events whose payload is `gains_db[TAS58XX_EQ_BANDS]`, and the listener calls `settings_set_eq_gains(gains_db[SETTINGS_EQ_BANDS])`. If anyone ever changes one to 10 or 31 bands, you get silent buffer over/under-runs — the type system can't catch it because float arrays decay. The TAS58XX constant in particular leaks an old hardware concept into a project that no longer ships TAS58XX.
**Suggested fix:** One canonical `AUDIO_EQ_BANDS` in `audio_eq.h`. Have `eq_events.h` and `settings.h` `#include "audio_eq.h"` and re-use it. Delete the `#ifdef CONFIG_DAC_TAS58XX / #include "dac_tas58xx_eq.h"` block in `eq_events.h:16-23` — the software EQ is hardware-agnostic now.
**Effort:** S.

### [4] `dac_tas58xx` is an unconditional dep of `main`
**Where:** `main/CMakeLists.txt:94`, `components/dac_tas58xx/CMakeLists.txt:1-5` (compiles to empty when CONFIG is off).
**Issue:** `main` PRIV_REQUIRES `dac_tas58xx` even on Voice PE builds where `CONFIG_DAC_TAS58XX=n`. The `dac_tas58xx` component handles this by registering an empty SRCS list, but the dependency remains in the graph and the header `dac_tas58xx_eq.h` is exposed via `INCLUDE_DIRS "."` so `eq_events.h` can include it (see Finding 3). This couples Voice PE's build to a chip family it never uses. Note that `dac_tas57xx` is *not* in the deps — inconsistent.
**Suggested fix:** After Finding 3, `dac_tas58xx` has no consumer in Voice PE. Remove it from `main/CMakeLists.txt:94`. If you want to preserve "build the same `main/` for an Esparagus board", make the dep conditional: `if(CONFIG_DAC_TAS58XX) list(APPEND DEPS dac_tas58xx) endif()` — same pattern already used for `bt`/`esp_eth` (`main/CMakeLists.txt:104-113`).
**Effort:** S.

### [5] Stale `set_power` API in docs and READMEs
**Where:** `docs/led-ux.md:5-9` ("ring's VCC rail is gated on GPIO45 … When the slide … is in the mute position the rail is driven HIGH"), `components/ha_airplay_leds/README.md:49-58` (`ha_airplay_leds_set_power(bool)`), `components/ha_airplay_ui/README.md:11`.
**Issue:** Commit `e66be29` made the slide gate decorative effects only, with the rail held HIGH for the device's lifetime (`leds.c:56-58`, `led_switch.c:1-12`). Three docs still describe the old behaviour, including a public API symbol (`ha_airplay_leds_set_power`) that no longer exists in `ha_airplay_leds.h`. A reader following `led-ux.md` will think the slide kills the LEDs, file bug reports, or — worse — start a refactor based on the wrong contract.
**Suggested fix:** Rewrite `docs/led-ux.md` "Power" section to describe the decorative gate; update `components/ha_airplay_leds/README.md` API table to reflect the actual surface (`ha_airplay_leds_set_decorative_enabled` / `_decorative_is_enabled`); update `components/ha_airplay_ui/README.md:11` to remove the `set_power` reference. Same edit pass that fixes Finding 6 below.
**Effort:** S (docs only).

### [6] `docs/architecture.md` predates the EQ + web-dashboard + chime additions
**Where:** `docs/architecture.md:1-117` vs commit `431cceb`.
**Issue:** The component graph diagram (lines 14-70) shows `playback_task → audio tap → I²S` but no `audio_eq`, no `chime`, no `now_playing`, no `web_server` data flow. The Phase map (lines 111-117) lists EQ, dashboard, chime, hostname slug under "planned" Phase 3 even though they shipped in Phase 1 last week. The "Concurrency" table omits `audio_eq` (which compiles coefficients off-task) and the `chime` consumer in `playback_task`. New contributors using this doc as a map will miss half the live system.
**Suggested fix:** Update the ASCII diagram to insert `audio_eq → audio_tap` between volume and I²S, add a `web_server` box on the right driving `eq_events`, mark Phase 1 entries as ✅ and document EQ/dashboard/chime under "Recently shipped" instead of "planned". Add `audio_eq` to the concurrency table (calling-task context, not its own task).
**Effort:** S.

### [7] Upstream `main/led.c` is dead weight on the Voice PE binary
**Where:** `main/led.c:1-560`, `main/main.c:223` (`led_init();`), `main/audio/audio_output.c:98,113,118` (`led_audio_feed(...)` calls), `sdkconfig.defaults.ha_voice_pe:39-41` (all three LED GPIOs = -1), `components/boards/ha_voice_pe/iot_board.h:35-37`.
**Issue:** ~560 lines of legacy single-LED + RGB-strip status driver compile into every Voice PE build. Every code path is guarded by `CONFIG_LED_*_GPIO >= 0`, all of which are `-1` on Voice PE, so `led_init()` is a chain of `ESP_LOGI` no-ops, `led_audio_feed` early-returns 30 Hz, and `rtsp_events_register` adds a permanently-idle listener. Dead, but it lives in the hot path — `audio_output.c` calls `led_audio_feed` three times per frame.
**Suggested fix:** Either (a) drop `led.c` from `main/CMakeLists.txt` SRC_FILES and the three call sites in `audio_output.c` and `main.c` for the Voice PE build (`if(NOT CONFIG_BOARD_HA_VOICE_PE)`), or (b) replace `main/led.c` with a five-line stub that no-ops the public API. (a) is cleaner; (b) keeps the option of cherry-picking upstream changes.
**Effort:** S.

### [8] `main/buttons.c` is similarly dormant
**Where:** `main/buttons.c:200-204`, `main/main.c:334`, `sdkconfig:633-637` (all `CONFIG_BTN_*_GPIO=-1`).
**Issue:** Voice PE drives buttons through `components/ha_airplay_ui/button.c` (multi-click + long-press on GPIO0). The upstream `buttons.c` (interrupt + debounce + 5 GPIOs from Kconfig) is compiled in but every GPIO is `-1`, so it allocates a 4-element queue and a task that never runs. Smaller debt than Finding 7 but the same shape: legacy code path that's only "off" by sdkconfig coincidence.
**Suggested fix:** Same as Finding 7 — gate `buttons.c` out of the Voice PE SRC_FILES list, or move it under `if(NOT CONFIG_BOARD_HA_VOICE_PE)`. The `buttons_init()` call in `main.c:334` should follow.
**Effort:** S.

### [9] `iot_board_init` doesn't init the jack-detect GPIO it advertises
**Where:** `components/boards/ha_voice_pe/iot_board.h:40` (`BOARD_JACK_GPIO 17`), `components/boards/ha_voice_pe/board.c` (no reference to it), `components/boards/ha_voice_pe/README.md:9` ("Phase 2 will flip low when the 3.5 mm jack is detected").
**Issue:** The header declares `BOARD_JACK_GPIO 17` and the comment says "200 ms debounce", but no code in the repo reads it. The amp-enable line (GPIO47) is initialised HIGH at boot (`board.c:213`) and never flipped — the architecture doc admits this on line 79 ("currently always powered on because jack-detect isn't wired yet"). This is fine for now, but the macro+comment promise more than the code delivers. Any user with both a jack plugged in and the internal speaker will get both outputs simultaneously.
**Suggested fix:** Either delete `BOARD_JACK_GPIO` from `iot_board.h` until Phase 2 wires it, or add a `// TODO(phase-2): jack detect on this pin` so a grep makes the gap explicit. Same for the "200 ms debounce" comment — it's spec, not implementation.
**Effort:** S (and gates Phase 2 which is the real fix).

### [10] `components/dac/dac.c` global-singleton ops is OK but the registration timing is implicit
**Where:** `components/dac/dac.c:10` (`static const dac_ops_t *s_ops = NULL;`), `components/boards/ha_voice_pe/board.c:232` (`dac_register(&dac_tlv320aic3204_ops);`), `main/main.c:228,238` (`iot_board_init` then `ha_airplay_leds_init`).
**Issue:** The `dac_register / dac_init` split assumes the board calls `dac_register` before *anyone* calls a `dac_*` function. `playback_control_init` is called at `main.c:222` (before `iot_board_init` at line 228) but happens to not touch the DAC — fragile. There's no assertion or log when a `dac_*` call lands on a NULL `s_ops`; failure mode is silent no-op, which is exactly how the volume-not-applied bugs in the upstream issue tracker manifest.
**Suggested fix:** Add an `ESP_LOGD` (or even `ESP_LOGW` on first miss) when `s_ops == NULL` in `dac.c:16-46`, and consider asserting in `dac_init` that `s_ops != NULL`. Cheap defensive guard, catches regressions where init order shifts.
**Effort:** S.

## Things that look healthy

- **`dac_ops_t` abstraction (`components/dac/include/dac.h:16-23`)** — clean six-function vtable, NULL-tolerant dispatcher, easy to add a future driver. The TLV320AIC3204 driver fits cleanly behind it.
- **`rtsp_events` and `eq_events` observer buses** — small, lock-free, well-scoped (`rtsp_events.c`, `eq_events.c`). No global state beyond the listener arrays. The same shape used twice independently is a good sign.
- **`audio_eq` double-buffered coefficient swap (`audio_eq.c:32-89, 200-249`)** — coefficient compute is off the audio task, publish is a single `atomic_store`, hot path is one `atomic_load` + branch. Textbook and correct.
- **`iot_board.h` baked-in pin map** — the README defends this choice well; the upstream Kconfig knobs are mirrored only as a compatibility shim. Don't unwind this.
- **`wifi_config.h` two-SSID fallback supervisor (`board.c:46-138`)** — small, idempotent, clean ownership (board layer seeds NVS, supervisor swaps on timeout). Matches the PRD constraint.
- **`ha_airplay_leds` / `ha_airplay_ui` split** — the boundary (LEDs render; UI dispatches RTSP/encoder/button events into LED state) is right. Don't merge them.
