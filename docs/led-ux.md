# LED UX spec

The 12-pixel WS2812B ring on GPIO21 is the primary visual surface of DenAir. Its behaviour is defined by a six-state machine driven by audio, UI, and AirPlay session events. See `components/denair_leds/README.md` for the implementation.

## Power

The ring's VCC rail is gated on GPIO45. When the slide on the side of the device (GPIO3) is in the **mute position** (active-low), the rail is driven HIGH and the ring powers up. In the opposite position the ring is dark — regardless of what the firmware *wants* to render. The slide's stock microphone-mute role is repurposed in DenAir because we don't use the microphone.

There's a 15 ms LDO-settle wait after power-on before the first frame is clocked; below that, the first few pixels render corrupted.

## States (highest priority first)

| Priority | State | Trigger | Look |
|---:|---|---|---|
| 1 | **MUTE** | Long-press of center button | Two dim red pixels at 3 and 9 o'clock. Persists until another long-press or an encoder turn. |
| 2 | **VOLUME overlay** | Encoder turn OR iPhone slider drag | Horizontal bar on pixels 1..12 proportional to volume (−30 dB..0 dB mapped to 0..12). Green below 8, yellow 8..10, red 10..12. Held for 2 s then fades to the state below. |
| 3 | **CONNECTION IN** | New AirPlay client | 5 s white rotating sweep: the lead pixel is bright white, the trailing two are dim, then blank. Rotates at 1 revolution / second. |
| 3 | **CONNECTION OUT** | AirPlay client disconnects | 2 s dim red fade-out, then falls through to IDLE. |
| 4 | **PLAYING** | AirPlay session is streaming | Audio-reactive beat-pulse: dim base colour fills the ring, whole ring flashes bright white on each detected beat (400 ms exponential decay), background brightness tracks full-band RMS. The base hue comes from the artwork if one is available and vivid, otherwise a slow rotation through the HSV wheel at 1 revolution / 20 s. |
| 5 | **IDLE** | No AirPlay session, not muted | 4 s amber breathing cycle (rises for 2 s, falls for 2 s). Peak brightness is deliberately low so a dark room isn't illuminated by the ring. |

Only the top applicable state renders. If you long-press (MUTE) while a volume change is still showing, MUTE wins; if the user releases MUTE, the rest of the stack re-applies based on what's currently happening.

## Beat detection

See `components/denair_leds/README.md` for the DSP detail. In short: the audio playback task taps the PCM stream and applies a one-pole low-pass at ~150 Hz to isolate bass energy. Each frame's energy is compared to a ~1.6 s exponential moving average; when instantaneous energy exceeds 1.40× the average AND at least 250 ms has passed since the last beat, a beat fires.

Typical kick drums and bass lines trigger reliably. Sustained pads and soft music do not, which is fine — the flash should feel like a rhythmic accent, not a constant pulse.

## Artwork → hue

AirPlay ships album artwork via RTSP SET_PARAMETER on every track change. `components/denair_artwork` decodes the JPEG at 1/8 scale (4096 pixels for a 512×512 source), computes mean RGB, and converts to HSV. If saturation > 0.18 and value > 0.12, the hue becomes the new PLAYING base colour. Below those thresholds the image is too muddy to extract a dominant hue from, and the engine falls back to the time-rotation.

The decode runs in a low-priority task on core 0 so it can't perturb audio. Latency from "iPhone shows new track" to "ring colour updates" is ~1 second in practice (RTSP artwork push + ~15 ms decode).

## Rationale

- **A quiet ring** during IDLE / PLAYING: the device sits in the living room; a constantly strobing visualiser would be intrusive. Rim lighting matching the album cover, with an occasional accent on each beat, feels present without being loud.
- **Loud overlays on events**: the CONNECTION sweep and VOLUME bar *should* be assertive — they tell the user that an action registered. 5 seconds is enough to confirm a connect; 2 seconds is enough for a volume change that you might be doing live alongside music playback.
- **MUTE as the top priority**: a persistent visible state that can't be hidden by transient events. Two dim red pixels are unambiguous without being loud.
- **Power-gating via the slide**: a mechanical off switch is more satisfying than any menu or long-press chord.
