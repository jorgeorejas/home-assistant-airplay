# Architecture

Home Assistant AirPlay is a fork of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) (AirPlay 2 receiver for ESP32-S3) with the following Home Assistant AirPlay-specific additions:

- Board support for the Home Assistant Voice Preview Edition (NC-VK-9727)
- TI TLV320AIC3204 codec/DAC driver, porting the ESPHome register sequence to native ESP-IDF
- Hard-coded WiFi credentials at compile time with a two-SSID fallback supervisor
- Audio-reactive WS2812B LED ring (12 pixels) on GPIO21 with beat detection + artwork-derived base hue
- Rotary encoder (volume), center button (play/pause + multi-click track control), and mute slide (LED on/off)

See `UPSTREAM-README.md` for the airplay-esp32 documentation that still applies to the RTSP/audio stack under `main/`.

## Component graph

```
                      ┌─────────────────────────────────────────┐
                      │                  main/                   │
                      │  RTSP · HAP · audio pipeline · PTP/NTP   │
                      │  mDNS · WiFi · playback_control · DACP   │
                      │  (upstream airplay-esp32, unmodified)    │
                      └───────┬──────────────────┬───────────────┘
                              │                   │
                        iot_board_init         playback events
                              │                   │
  ┌───────────────────────────┼───────────────────┼─────────────────────────┐
  │ components/ (Home Assistant AirPlay additions on top of airplay-esp32's DAC abstraction)│
  │                           │                   │                         │
  │      ┌────────────────────▼────────────────┐  │                         │
  │      │ boards/ha_voice_pe                  │  │                         │
  │      │  pin map, I2C bus, GPIO47 amp, GPIO3│  │                         │
  │      │  mute-slide, GPIO45 LED power rail, │  │                         │
  │      │  wifi_config.h seed + fallback task │  │                         │
  │      └──┬──────────────────────────────────┘  │                         │
  │         │ registers dac_ops_t                  │                         │
  │         ▼                                      │                         │
  │    ┌─────────────────────┐                     │                         │
  │    │ dac_tlv320aic3204   │                     │                         │
  │    │ I²C init, volume,   │                     │                         │
  │    │ power mode, routing │                     │                         │
  │    └─────────┬───────────┘                     │                         │
  │              │                                 │                         │
  │              │ I²S 44.1/48 kHz ───► AIC3204 ───► 3.5 mm jack + internal │
  │              │                                                            │
  │    ┌─────────▼───────────┐    ┌────────────────────┐    ┌──────────────┐ │
  │    │ ha_airplay_ui            │◄──┤ rtsp_events        ├───►│ ha_airplay_leds  │ │
  │    │ encoder (GPIO16/18), │    └────────────────────┘    │ WS2812 ring  │ │
  │    │ center button (GPIO0)│                              │ 6-state FSM  │ │
  │    │ multi-click detector,│    ┌────────────────────┐    │ beat-pulse   │ │
  │    │ mute slide (GPIO3) → │    │ audio_output       ├───►│ audio tap    │ │
  │    │ LED power (GPIO45)   │    │ playback_task      │    │ (bass LPF +  │ │
  │    └──────────┬───────────┘    └────────────────────┘    │  RMS EMA)    │ │
  │               │                                          │              │ │
  │               │ volume bar / mute indicator ────────────►│              │ │
  │                                                         ▲│              │ │
  │    ┌────────────────────┐                               ││              │ │
  │    │ ha_airplay_artwork     │  set_base_hue ────────────────┘│              │ │
  │    │ tjpgd in ROM,      │                                │              │ │
  │    │ queued decode task │                                │              │ │
  │    └─────────▲──────────┘                                └──────────────┘ │
  │              │                                                            │
  └──────────────┼────────────────────────────────────────────────────────────┘
                 │
                 │ JPEG bytes
                 │
       ┌─────────┴───────────┐
       │ main/rtsp           │
       │ SET_PARAMETER       │
       │ image/jpeg hook     │
       └─────────────────────┘
```

## Data flow (steady state, music playing)

1. **iPhone → RTSP** → `main/rtsp/` parses frames, decrypts, decodes ALAC.
2. **Decoded PCM** goes through `main/audio/audio_buffer.c` (sorted jitter buffer) into `playback_task`.
3. `playback_task` resamples if needed, applies digital volume curve, then **before** writing I²S:
   - calls `led_audio_feed()` (upstream single-LED indicator, no-op here — GPIO=-1)
   - calls `ha_airplay_audio_tap()` (our bass-band RMS + beat detector)
4. I²S master output on GPIO7/8/10 → TLV320AIC3204 → 3.5 mm jack (and internal amp on GPIO47 when the jack is not inserted — currently always powered on because jack-detect isn't wired yet).
5. `ha_airplay_leds` render task runs at 50 Hz on core 1. It reads the tap's atomic energy / beat values, plus the artwork-derived hue, and renders one of six states (mute / volume / connection-in / connection-out / playing / idle).
6. RTSP session events (`CLIENT_CONNECTED`, `PLAYING`, `PAUSED`, `DISCONNECTED`) are subscribed via `rtsp_events_register()` by `ha_airplay_ui/ui.c`, which drives `ha_airplay_leds_set_playback_state()` and `ha_airplay_leds_flash_connection()`.
7. Artwork `SET_PARAMETER` (`image/jpeg`) is forwarded to `ha_airplay_artwork_update()`. That copies the JPEG into PSRAM and posts to a decoder task; the task runs tjpgd (ESP32-S3 ROM) at 1/8 scale, sums RGB across the decoded rectangles, converts mean → HSV, and calls `ha_airplay_leds_set_base_hue()`.

## Concurrency

| Task                    | Core | Priority | Purpose                               |
|-------------------------|------|---------:|---------------------------------------|
| `audio_play`            | 1    | 7        | Resample + I²S write + audio tap      |
| `ha_airplay_leds`           | 1    | 3        | 50 Hz pixel render                    |
| `ha_airplay_art`            | 0    | 3        | JPEG decode on track change           |
| `ha_airplay_enc`            | any  | 10       | Quadrature state machine              |
| `ha_airplay_btn`            | any  | 8        | Multi-click + long-press              |
| `ha_airplay_sw` (LED power) | any  | 6        | Mute slide state change               |
| `ha_airplay_volpoll`        | any  | 4        | 5 Hz NVS-cache poll for iOS slider    |
| `wifi_scan`             | 0    | 3        | One-shot scan+connect on STA start    |
| Upstream RTSP/HAP/PTP tasks (see airplay-esp32 README)                |

Core 0 runs WiFi and the full AirPlay protocol stack. Core 1 runs the audio playback task; the LED render task shares core 1 but at much lower priority so it cannot starve audio.

## Key design decisions

- **Framework**: pure ESP-IDF v5.4.1 — no ESPHome runtime overhead. Required to fit AirPlay 2 + WiFi + UI within the <100 KB free internal heap gate (Phase 0). See `phase0-report.md`.
- **AirPlay 2 (default)**: upstream's AirPlay 2 stack is what iOS negotiates today. We briefly tried `CONFIG_AIRPLAY_FORCE_V1=y` to get encoder → iPhone slider sync via DACP, but modern iOS no longer sends DACP-ID to non-MFi devices. Flipped back. Device → iPhone sync is an accepted protocol limit.
- **Hardware volume**: AIC3204 digital-volume registers (page 0, regs 65/66). Software scaling before the DAC would cost SNR headroom.
- **Hard-coded WiFi** (per PRD): credentials live in a git-ignored `wifi_config.h` that the board layer seeds into NVS on every boot. Two-SSID fallback swaps after 15 s if the primary isn't visible.
- **Link order trick**: Home Assistant AirPlay init calls (`ha_airplay_leds_init`, `ha_airplay_ui_init`, `ha_airplay_artwork_init`) live in `main/main.c`, not in `boards/ha_voice_pe/board.c`, because ESP-IDF's PRIV_REQUIRES ordering puts the ha_airplay_* libs before `libboards.a` on the linker's single pass. Putting references in `libmain.a` (linked last) sidesteps the undefined-reference.
- **JPEG decoding in ROM**: ESP32-S3 has tjpgd built into ROM — zero flash cost, ~15 ms per 512×512 artwork at 1/8 scale. See `components/ha_airplay_artwork/README.md`.

## Phase map

| Phase | Status   | Scope                                                          |
|-------|----------|----------------------------------------------------------------|
| 0     | ✅ GREEN | Viability PoC: AIC3204 audio out, AirPlay discover/stream, heap gate. [phase0-report.md](phase0-report.md) |
| 1     | ✅       | Encoder volume, button controls, WS2812B ring with beat-pulse + artwork hue, mute slide → LED power. [phase1-report.md](phase1-report.md) |
| 2     | planned  | Jack-detect auto-switch (GPIO17 → GPIO47), spectrum LED mode, long-press mode cycle |
| 3     | planned  | OTA polish, boot <8 s, soak test, production artwork cache     |
