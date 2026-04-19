# `denair_ui`

The physical-controls layer: rotary encoder, center button, and the side slide switch. Translates GPIO events into calls on the upstream playback-control API and into LED-state events.

## Split

| File | GPIO | Responsibility |
|---|---|---|
| `encoder.c` | 16 (A), 18 (B) | Quadrature decode — ISR notifies a task that walks a 16-entry state table. Fires on every detent (4 edges). |
| `button.c` | 0 | Center push-button. Debounce → multi-click counter → long-press detector. One of `short / double / triple / long` callbacks fires per interaction. |
| `led_switch.c` | 3 | Side slide switch. ISR + debounce task. LOW → LEDs on (drives `denair_leds_set_power(true)`), HIGH → LEDs off. |
| `ui.c` | — | Coordinator. Wires the above into `playback_control_*`, `denair_leds_*`, `rtsp_events_*`, and maintains the hard-mute state machine. |

Public API is a single `denair_ui_init()` in `include/denair_ui.h`. `ui_internal.h` defines the encoder/button/switch-start signatures kept private to the component.

## Button gestures

All four gestures exist so the device UX doesn't feel crippled by iOS having dropped DACP for non-MFi AirPlay receivers:

| Gesture | Callback | Effect on device | Effect on iPhone |
|---|---|---|---|
| Short press | `on_button_short` | Local pause/unpause via upstream's `playback_control_play_pause` (mute fallback in AP2 with no DACP) | None |
| Double click (≤400 ms between releases) | `on_button_double` | `playback_control_next()` → tries to send DACP NEXT | None (iOS ignores DACP) |
| Triple click | `on_button_triple` | `playback_control_prev()` → tries DACP PREV | None |
| Long press (≥1.5 s held) | `on_button_long` | Toggles `dac_set_power_mode(STANDBY↔ON)` + `denair_leds_set_muted()` | None |

The next/prev attempts are harmless no-ops on modern iOS. If Apple ever re-enables MFi-free DACP or an MFi stack lands upstream, the gestures will start reaching the iPhone without any code change here.

## Encoder turn

Every detent rotates through upstream's `playback_control_volume_up / _volume_down`, which:

1. Reads the current AirPlay-scale volume (-30..0 dB) from NVS.
2. Adjusts by ±3 dB.
3. Writes the new value back to NVS and calls `dac_set_volume(new)`.
4. Attempts a `dacp_send_volume()` push to the iPhone (no-op on modern iOS).

Our wrapper in `on_encoder_turn`:

1. Clears hard-mute if it was set (turn-to-unmute is intuitive).
2. Triggers a 2 s volume-bar overlay on the LED ring with the current fraction.

## Volume poll task

iPhone-side volume changes don't go through our encoder path — they arrive via RTSP SET_PARAMETER, which upstream handles in `rtsp_conn_set_volume` and writes to NVS. Since upstream doesn't emit an event for this, we poll `settings_get_volume()` at 5 Hz from `volume_poll_task`. When it drifts by more than 0.5 %, we fire the same 2 s volume bar — so the ring lights up whether the change came from the dial or from iOS.

## AirPlay event subscription

`denair_ui_init()` registers a callback via `rtsp_events_register()` (upstream observer pattern). Mapping:

| Event | LED action |
|---|---|
| `RTSP_EVENT_CLIENT_CONNECTED` | `denair_leds_flash_connection()` (5 s sweep) + state → CONNECTED |
| `RTSP_EVENT_PLAYING` | state → PLAYING (beat-pulse kicks in) |
| `RTSP_EVENT_PAUSED` | state → PAUSED (ring returns to idle breath) |
| `RTSP_EVENT_DISCONNECTED` | state → DISCONNECTED (2 s red fade then idle) |
| `RTSP_EVENT_METADATA` | unused here (artwork handling is in `denair_artwork`) |

## LED on/off slide (`led_switch.c`)

Stock HA Voice PE firmware uses this slide as a hardware microphone mute — DenAir doesn't use the microphone at all, so the slide is free. The polarity matches the user's intuition: slide physically *toward* the mute icon → ring lights up; slide away → ring goes dark.

- 40 ms debounce task (the slide bounces noticeably on the physical mechanism).
- Seeds the power state at boot from the current GPIO level so the ring reflects the slider position as soon as `denair_leds_init` finishes.
- Uses `gpio_install_isr_service()` — tolerates the case where it's already installed (the encoder and button components install it first).

## Stack sizes

| Task | Stack | Priority | Core |
|---|---:|---:|:---:|
| `denair_enc` | 3072 B | 10 | any |
| `denair_btn` | 3584 B | 8 | any |
| `denair_sw` (mute slide) | 3072 B | 6 | any |
| `denair_volpoll` | 3072 B | 4 | any |

All GPIO ISRs use `vTaskNotifyGiveFromISR` so they execute in microseconds; the user-space tasks do the deferred work.
