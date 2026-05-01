# `ha_airplay_leds`

The 12-pixel WS2812B ring engine. Renders at 50 Hz on core 1, reads from a handful of atomic state variables set by the UI / artwork / AirPlay-event paths, and drives the pixels over RMT.

See [`docs/led-ux.md`](../../docs/led-ux.md) for the state-machine spec from the user's perspective.

## Pieces

### `leds.c` — the render loop + state machine

A single `led_task` on core 1 wakes every 20 ms and decides what to draw:

```c
if (muted)                                 → render_muted()
else if (volume-overlay hold active)        → render_volume_bar()
else if (connection-in overlay active)      → render_conn_in()
else if (connection-out overlay active)     → render_conn_out()
else if (playback state is PLAYING)         → render_playing()
else                                        → render_idle()
```

Each render function only writes pixels; actual transmission happens once per frame via `led_strip_refresh()`. The task skips the refresh when the VCC rail (GPIO45) is off, so no corrupted bytes are clocked into an unpowered ring.

### `audio_tap.c` — bass-band RMS + beat detector

Called from `main/audio/audio_output.c:playback_task` right before `i2s_channel_write`. Given a frame of int16 stereo PCM:

1. Downmix to mono.
2. Accumulate full-frame RMS² for the ambient-brightness signal.
3. Apply a one-pole low-pass at ~150 Hz (IIR, `y[n] = y[n-1] + α(x[n] - y[n-1])`, α = 0.0195 ≈ Q15 640). Accumulate bass RMS².
4. EMA the bass-RMS² over ~1.6 s. If instantaneous > EMA × 1.40 AND at least 250 ms since the last beat, fire a beat.
5. Publish the scaled RMS, bass energy, and last-beat timestamp via atomics.

Everything runs in the audio task's own context. No queues, no FFT. ~200 arithmetic ops per stereo frame.

### `leds_internal.h`

Private glue between `audio_tap.c` and `leds.c` so their contract isn't exposed to the rest of the firmware.

## Rendering details

- **Idle** — cos-shaped breath over 4 s. Peak amber (40, 16, 0) is deliberately dim: the ring is ambient lighting, not a lamp.
- **Volume bar** — colour is green up to position 8, amber at 8-9, red at 10+. Mirrors the peak-hold convention of mixer meters.
- **Connection in** — a single "lead" pixel rotates around the ring at 1 rev/s, with a decaying tail (1.0 at lead, 0.5 at −1, 0.2 at −2, 0.05 elsewhere). Fades to zero in the last 500 ms.
- **Connection out** — ring flashes dim red and fades to zero over 2 s.
- **Playing** — base HSV colour at `base_bright = 0.15 + 0.20 × RMS`, hue from artwork when available else time-rotating. On beat: add `flash × 180` to every RGB channel, flash decays with τ = 400 ms. The saturation/gain lets music "move" the ring without being loud.
- **Muted** — fixed: positions 3 and 9 at (60, 0, 0), everything else off. Mirrors the upstream yaml's muted pattern.

## Power rail and decorative gate

The WS2812B VCC rail (GPIO45) is driven HIGH at `ha_airplay_leds_init()` and held there for the device's lifetime. The render task sleeps 15 ms (`LED_POWER_SETTLE_MS`) after the boot rise before clocking the first frame, giving the LDO and bypass cap time to stabilise — without the settle the first frame shows a garbled green artifact.

`ha_airplay_leds_set_decorative_enabled(bool)` toggles a software flag that gates only the PLAYING and IDLE renders. Utility overlays (MUTE, VOLUME bar, CONNECTION sweep) render in either state, so the user keeps essential feedback when the slide is "off" — only the ambient/idle effects go dark. When decorative is off and no utility overlay is active, the render task writes blank pixels.

## Public API (`include/ha_airplay_leds.h`)

| Symbol | Called from | Effect |
|---|---|---|
| `ha_airplay_leds_init()` | `main/main.c` | creates the RMT strip handle, arms GPIO45 HIGH, starts the render task |
| `ha_airplay_leds_set_decorative_enabled(bool)` | `components/ha_airplay_ui/led_switch.c` (slide) | gate decorative renders |
| `ha_airplay_leds_set_playback_state(state)` | `components/ha_airplay_ui/ui.c` (RTSP events) | idle / connected / playing / paused / disconnected |
| `ha_airplay_leds_show_volume(fraction, hold_ms)` | encoder and iOS-volume poll | volume overlay |
| `ha_airplay_leds_flash_connection()` | `RTSP_EVENT_CLIENT_CONNECTED` | 5 s sweep |
| `ha_airplay_leds_set_muted(bool)` | long-press button | mute indicator |
| `ha_airplay_leds_set_base_hue(hue_deg, enabled)` | `components/ha_airplay_artwork/artwork.c` | override PLAYING base colour |
| `ha_airplay_audio_tap(samples, stereo_frames)` | `main/audio/audio_output.c:playback_task` | feed PCM to the beat detector |

## Why core 1

Core 0 is where WiFi, mDNS, RTSP, and the full AirPlay protocol stack live. Render overhead is small (~50 μs per frame) but we don't want it fighting the RTSP state machine. Core 1 runs the audio playback task at priority 7; the LED render task is priority 3 so it can never starve audio. Beat jitter is visible up to ~20 ms of delay; the task's 20 ms tick plus variable wake latency keeps it comfortably under that.
