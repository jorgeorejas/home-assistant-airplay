# UX & feature gaps review

**Scope:** End-user experience and feature completeness of Home Assistant AirPlay (commit `e66be29`, branch `phase0-raop`) — what shipped vs the PRD/phase plan, plus opinionated gaps in the dashboard, LED ring, encoder, button, chime, and setup flow.
**Date:** 2026-05-01

> Note on sources: the canonical PRD is not checked into the repo (the phase reports cite `~/.claude/plans` and PRD §5.x without a repo copy). This review takes the PRD requirements as enumerated in `docs/phase0-report.md`, `docs/phase1-report.md`, and `docs/architecture.md` as ground truth for "what was promised."

## TL;DR

- **The dashboard is the weakest link.** The home page polls `/api/now_playing` every 2 s and `/api/system/info` every 10 s — there's no WebSocket push, so the visible "play" lag from phone-tap to UI update can feel sluggish (median ~1 s, worst-case ~2 s). Worse, `/api/eq` POST has zero error toasting beyond a try/catch, no debounce indicator, and no "saved" persistence indicator on reload.
- **No artwork on the dashboard.** We already decode artwork on-device for LED hue, but the homepage shows an empty grey "Now playing" card while the iPhone shows a beautiful album cover. This is the single highest-leverage delight upgrade.
- **The slide switch is a discoverability landmine.** Stock firmware = mic mute. Our firmware = "decorative LED off." A first-time user looking at the device has zero way to know this — there's no label, no LED, no dashboard surfacing of slide state.
- **Phase 2 jack-detect is more user-visible than spectrum mode.** Currently the internal speaker is *always* powered (GPIO47 hard-high). Plug in headphones or an aux cable and the internal driver keeps running — wasted power, possible crosstalk hum, and a worse UX than the stock unit.
- **Setup is not viable for non-developers today.** "Edit `wifi_config.h` and flash via ESP-IDF" is the documented happy path. The captive portal at 192.168.4.1 is wired but invisible: no docs, no published flow, and the LED ring gives zero hint that the device is in AP-only mode.

## Gaps vs PRD

| Requirement (per PRD / phase reports) | Status | Notes |
|---|:-:|---|
| Phase 0: AirPlay 2 viability, ≥100 KB heap | ✅ | 168 KB measured |
| Phase 0: Audio at 3.5 mm jack | ⏳→✅ | Phase 1 confirmed audible |
| Phase 0: Latency ≤300 ms | ⏳ | Never bench-measured |
| Phase 1 §5.2: WiFi two-SSID fallback | ✅ | Compile-time only, no UI to edit |
| Phase 1 §5.3: Jack-detect → amp toggle | ❌ | GPIO17 not read; GPIO47 stuck HIGH. **Should be Phase 2-now.** |
| Phase 1 §5.3: 5 s output-destination LED | ❌ | Tied to jack-detect |
| Phase 1 §5.4: Encoder volume, ~3 dB/click | ✅ | 3 dB exactly; see finding L2 |
| Phase 1 §5.4: Mute (long-press) | ✅ | Works; LED indicator clear |
| Phase 1 §5.4: Volume persistence | ✅ | NVS-backed |
| Phase 1 §5.5: WS2812B 4-state ring | ✅+ | 6-state machine; spectrum/VU absent |
| Phase 2: Persistent LED mode in NVS | ❌ | No mode-cycling at all yet |
| Phase 2: Spectrum LED mode | ❌ | Lower priority IMO — see L8 |
| Phase 3: Boot < 8 s | ⏳ | Not measured; up to 5 s ethernet wait + 30 s wifi wait suggests we're over |
| Phase 3: Soak test (≥30 min) | ⏳ | Never recorded |
| Phase 3: OTA polish | ⚠️ | Endpoint exists (`/api/ota/update`) but no UI button, no progress, no rollback path documented |
| Track-change LED transition (Phase 2) | ❌ | Hue updates instantly; no animation |
| Now-playing endpoint | ✅ | Polled, not pushed; see L1 |
| Web-editable device name | ⚠️ | API exists, dashboard does not surface it; see L4 |
| Web-editable WiFi (post-setup) | ⚠️ | API exists, no UI |
| Connection chime | ✅ bonus | Not in PRD; see L5 |
| EQ (15-band, 5 presets) | ✅ bonus | Not in PRD; see L3 |

## Findings (current UX)

### [HIGH] L1 — Now-playing has visible polling lag
**Where:** `main/network/index.html:209`, `main/network/web_server.c:82-110`, `main/now_playing.c`
**Issue:** `setInterval(refreshNp, 2000)` means the homepage can take up to 2 s to reflect play/pause/track change after the phone fires. RTSP events update `s_state` instantly via `now_playing.c:on_event`, but the dashboard polls cold. The log viewer already proves the pattern works for push (`/ws/logs` over WebSocket, `log_stream.c`). Polling at 2 s also generates ~30 idle requests/minute when the page is open, which is wasteful.
**Suggested fix:** Add `/ws/now_playing` that pushes a JSON frame on every `RTSP_EVENT_*` and on every `metadata` update, mirroring `log_stream`'s pattern. Keep `/api/now_playing` for initial render. Same trick gives free progress-bar smoothing because we know the position-update rate.
**Effort:** S (~150 LOC, mirrors `log_stream`).

### [HIGH] L2 — No artwork on the homepage
**Where:** `main/network/index.html:60`, `components/ha_airplay_artwork/`
**Issue:** The artwork decoder already has the JPEG in PSRAM (it's used to extract the LED hue). The homepage renders an empty grey "Now playing" card while the same album cover sits decoded in RAM. This is the single most "iPhone-y" delight win available — and `now_playing_t` already has a `has_artwork` boolean exposed in `/api/now_playing`. We just don't serve the bytes.
**Suggested fix:** Cache the most recent artwork JPEG (max ~200 KB) in the artwork component, expose `/api/artwork.jpg` with `Cache-Control: max-age=0, must-revalidate` and a per-track ETag built from the metadata-hash. Add a 96×96 thumbnail in the now-playing card with a soft drop shadow.
**Effort:** S.

### [HIGH] L3 — EQ commit has no real feedback loop
**Where:** `main/network/index.html:130-139`
**Issue:** The 180 ms debounce is fine, but: (a) the toast says "EQ saved" *before* the DAC has confirmed application — the POST returns success once the event is queued, not once the audio path is updated; (b) the slider snaps to integer 1 dB steps so dragging looks janky compared to iOS-quality EQ UIs; (c) there's no visual diff between "this matches a preset" and "this is custom" — pressing **Loudness** then nudging one band leaves all five preset buttons looking identical; (d) reload shows the saved gains but doesn't highlight which preset (if any) is active. POST-failure path silently shows "Save failed" with no log of what was attempted.
**Suggested fix:** (i) show a "live"/"applied" pill that flips green only after the next `/api/eq` GET round-trips successfully; (ii) drop step to 0.5 dB; (iii) highlight the matching preset button (compute exact-match against the preset table on apply); (iv) on POST failure, show the actual error in the toast for ≥4 s, not 1.5 s.
**Effort:** M.

### [HIGH] L4 — Friendly name lives in code, not in the dashboard
**Where:** `wifi_config.h.example:12`, `main/network/web_server.c:223-263`, `main/network/index.html` (no UI)
**Issue:** "Altavoces Salón" is hardcoded into `wifi_config.h`, seeded into NVS on every boot (per commit `0d17da7`). The web server *has* `/api/device/name` POST but the dashboard never exposes an input. So the field user has no way to rename. Worse, every boot overwrites whatever the user might have set via API — the seed-on-boot is a feature gap waiting to bite. A first-time owner who doesn't read source can't change "Home Assistant AirPlay" to "Living Room".
**Suggested fix:** (i) Add a Device card with editable name + Save button that calls `/api/device/name`; (ii) make boot-time NVS seed a one-shot (factory-default flag) instead of every-boot — only seed if NVS is empty or a `factory_reset` flag was set. (iii) Surface the mDNS hostname (`<slug>.local`) in the same card so the user knows what to type into Safari.
**Effort:** M.

### [MEDIUM] L5 — Chime fires on CONNECTED, then volume yanks under it
**Where:** `main/main.c:46-53`, `main/audio/chime.c`, `main/audio/audio_output.c:104-116`
**Issue:** Chime triggers on `RTSP_EVENT_CLIENT_CONNECTED`, which is *before* the iPhone has negotiated codec / volume / etc. The chime PCM is played at full digital scale through `i2s_channel_write` — `apply_volume` is **not** called on the chime path (see `audio_output.c:96` vs `:115`). So at 0 dB on the dial the chime is fine, but at -30 dB the chime is jarringly loud relative to the music that follows. The chime also doesn't yield gracefully when audio frames arrive — `chime_consume` is only checked when `samples == 0`, so once RTSP starts streaming the chime gets cut mid-arpeggio.
**Suggested fix:** (i) Apply the same `apply_volume` to chime PCM before I²S write so the chime obeys the dial; (ii) move the chime trigger to `RTSP_EVENT_PLAYING` (first frame) so it doesn't compete with the pre-roll, OR explicitly drain remaining chime frames with a 30-ms crossfade when receiver-frames arrive.
**Effort:** S.

### [MEDIUM] L6 — Slide-switch state is invisible and undocumented to the user
**Where:** `components/ha_airplay_ui/led_switch.c`, `docs/led-ux.md:7`
**Issue:** A user holding the device sees a slider labeled (or, on this rev, unlabeled) for "mic mute". On our firmware it does nothing for the mic — it gates decorative LED renders. There's no visible affordance: no UI surfacing in the dashboard, no LED indicator (the slide-off state literally turns off the LEDs that would tell you about state), and the "decorative-only" semantic is subtle (mute/volume/connection still render, idle/playing don't).
**Suggested fix:** (i) Surface "decorative LEDs: ON / OFF" in the dashboard's Status card with a software toggle that mirrors the slide; (ii) on slide change, render a 1 s toggle confirmation animation on the ring (e.g. 12 dim white pixels fade to off, or two pixels at 12-o'clock blink amber); (iii) update README and add a small docs section explicitly noting the slide is repurposed.
**Effort:** S.

### [MEDIUM] L7 — Captive portal exists but is unreachable in practice
**Where:** `main/main.c:301`, `main/network/web_server.c:113-137`
**Issue:** The fallback path is `wifi_config.h` → reflash. The captive portal exists (handlers wired for Apple/Android/Windows detection at `web_server.c:113+`), but: (a) the docs say "edit `wifi_config.h`" full stop — the portal is not in the user-facing flow; (b) there's no LED state for "in AP-only mode, waiting for setup" (we'd fall through to IDLE breath, indistinguishable from "just sitting there"); (c) `iot_board_init`'s seed-on-boot will overwrite NVS-saved credentials with whatever was compiled in, so a user who connects to the AP and saves credentials will lose them on the next reboot.
**Suggested fix:** Either commit to compile-time-only and rip out the AP/captive portal (smaller binary, simpler), or commit to the portal: (i) only seed NVS from `wifi_config.h` if NVS is empty or a `factory_reset` flag set, (ii) add a distinctive LED state ("AP setup mode" = slow purple breath), (iii) document the portal flow in the README with an actual screenshot.
**Effort:** M (decide direction first).

### [MEDIUM] L8 — Volume curve is coarse at the loud end
**Where:** `main/playback_control.c:34`, `components/ha_airplay_ui/encoder.c:65`
**Issue:** 3 dB per detent across a 30 dB range = 11 detents end-to-end. From a comfortable listening level (-12 dB) one click takes you to -9 dB → that's a *noticeable* jump (≈40 % SPL). At low volumes 3 dB feels right; at louder ones it's coarse. Also: the volume bar lights up green/yellow/red bands but "red = loud" is the inverse of the iPhone's slider colour semantic, where red would mean "mic on" or "danger" — slightly confusing.
**Suggested fix:** (i) Switch to a non-linear curve: 3 dB per detent below -18 dB, 1.5 dB above. Cheap implementation: pass an extra parameter to `airplay_adjust_volume(step_db)` that picks the step from a piecewise table. (ii) Drop the red zone — keep the bar all-green with a subtle gradient, or make >-6 dB pulse once per click as the "loud zone" cue.
**Effort:** S.

### [LOW] L9 — Missing visible states (paused / connecting-failing / OTA / restart-confirmed)
**Where:** `components/ha_airplay_leds/leds.c`, `main/network/index.html`
**Issue:** State coverage gaps: (a) **paused** vs **idle** look identical on the ring (`leds.c:255` falls through to `render_idle` for both `PAUSED` and `IDLE`) — dashboard distinguishes them but the device alone does not; (b) there's no "connecting / negotiation failed" state — if the phone times out mid-handshake, the user sees nothing; (c) OTA via `/api/ota/update` shows zero progress on the dashboard (no UI button at all in fact); (d) the Restart button shows a "Restarting…" toast for 1.5 s and then the page silently 502s while the device reboots, with no auto-reconnect logic.
**Suggested fix:** (i) Render PAUSED as the playing colour at 50 % brightness with no beat-flash — clear "stalled" semantic; (ii) add an OTA card to the dashboard (file picker + progress bar driven by upload progress events); (iii) after Restart toast, poll `/api/system/info` every 1 s and only re-render when it comes back up — show "Reconnecting…" until then.
**Effort:** M.

### [LOW] L10 — Documentation drift
**Where:** `docs/hardware.md:42`, `docs/led-ux.md:7`, `README.md:29`
**Issue:** Three places say the slide drives the WS2812B VCC rail (GPIO45). Per commit `e66be29`, GPIO45 is now driven HIGH at init and stays HIGH; the slide drives a software flag (`s_decorative_enabled`). `docs/hardware.md` row "LED ring VCC enable | 45 | out | **driven by the mute slide**" is now flat-out wrong. Same for `led-ux.md` ¶ "Power" and `README.md`'s feature bullet. `docs/architecture.md:7` also still says the slide is "LED on/off" rather than "decorative on/off."
**Suggested fix:** Sweep all three files. Add a sentence to `led-ux.md` clarifying utility-overlay-always-renders behaviour. While in there, fix the `phase1-report.md` line "Tested live: Asturias … `Wireless_CASA`" which leaks the user's specific SSIDs.
**Effort:** S.

## "Delight" candidates (would-be-cool, not promised)

1. **Artwork on the homepage** (covered in L2) — single highest-impact change. Decoded JPEG already lives in PSRAM; just expose it.
2. **Track-change ring sweep** — when `RTSP_EVENT_METADATA` brings a new `title`, run a 1.5 s rotating shimmer in the new artwork hue. Free transition that masks the "abrupt hue change" the renderer currently does mid-frame.
3. **Sleep timer** — a card on the dashboard ("Stop in 30 / 60 / 120 minutes") that fades volume to -30 dB over the last 60 s, then mutes. Very low effort (`esp_timer` + the existing `dac_set_volume`), high "this device cares about my evening" delight.
4. **Bigger "AirDrop-style" connect animation** — when a NEW client (different IP from last session) connects, render the rotating sweep at 2× brightness for the first 2 seconds. Zero-cost differentiation from "same phone reconnecting after hiccup."
5. **mDNS-discoverable web UI link** — advertise `_http._tcp.` alongside the AirPlay record so Safari's "Network" sidebar lists `Altavoces Salón` and clicking opens the dashboard. Currently a user has to know to type `altavoces-salon.local` into the URL bar.

## Healthy

- **Live log viewer** (`/logs`, `log_stream.c`) is genuinely good — WebSocket-driven, ANSI-stripping, level coloring, pause / clear / filter / autoscroll. Honestly the most polished part of the firmware UI. It's also the template for fixing L1.
- **Beat-pulse + artwork hue** is the kind of thing that makes the device feel premium. The vivid-only gate (saturation > 0.18) is a *nice* taste call — it correctly avoids muddy almost-grey covers turning the ring into a dim brown stain.
- **Long-press → DAC standby mute** is a smart choice. The hard-mute survives play/pause and is cleared by an encoder turn (which matches every iPod / iPhone / Walkman muscle memory ever shipped). The 1.5 s threshold is well-tuned — long enough to be intentional, short enough to not feel like a chore.
