# Architecture

Home Assistant AirPlay is a fork of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) (AirPlay 2 receiver for ESP32-S3) with the following Home Assistant AirPlay-specific additions:

- Board support for the Home Assistant Voice Preview Edition (NC-VK-9727)
- TI TLV320AIC3204 codec/DAC driver, porting the ESPHome register sequence to native ESP-IDF
- Hard-coded WiFi credentials at compile time with a two-SSID fallback supervisor
- Audio-reactive WS2812B LED ring (12 pixels) on GPIO21 with beat detection + artwork-derived base hue
- Rotary encoder (volume), center button (play/pause + multi-click track control), and slide switch that gates decorative LED renders (utility overlays still show)

See `UPSTREAM-README.md` for the airplay-esp32 documentation that still applies to the RTSP/audio stack under `main/`.

## Component graph

```
                      ┌──────────────────────────────────────────────┐
                      │                  main/                        │
                      │  RTSP · airplay_pair · audio pipeline · PTP   │
                      │  mDNS · WiFi · playback_control · OTA         │
                      │  + audio_eq · chime · now_playing             │
                      └───┬────────────┬────────────┬─────────────────┘
                          │            │            │
                    iot_board_init   rtsp_events  eq_events
                          │            │            │
  ┌───────────────────────┼────────────┼────────────┼─────────────────────┐
  │ components/ (Home Assistant AirPlay additions)                       │
  │                       │            │            │                     │
  │   ┌───────────────────▼─────┐      │            │                     │
  │   │ boards/ha_voice_pe      │      │            │                     │
  │   │  pin map, I²C bus,      │      │            │                     │
  │   │  GPIO45 (rail HIGH),    │      │            │                     │
  │   │  factory-seed-once NVS  │      │            │                     │
  │   └─────┬───────────────────┘      │            │                     │
  │         │ dac_ops_t                 │            │                     │
  │         ▼                           │            │                     │
  │   ┌──────────────────┐              │            │                     │
  │   │ dac_tlv320aic3204│              │            │                     │
  │   │ vol, power mode  │              │            │                     │
  │   └────┬─────────────┘              │            │                     │
  │        │ I²S 44.1/48 kHz ─► AIC3204 ─► 3.5 mm / internal speaker      │
  │                                                                        │
  │   ┌────────────────────┐  ┌─────────────┐  ┌──────────────┐           │
  │   │ ha_airplay_ui      │◄─┤ rtsp_events │─►│ ha_airplay_leds          │
  │   │ encoder, button,   │  └─────────────┘  │ WS2812 ring   │           │
  │   │ slide → decorative │                   │ 6-state + 5 s │           │
  │   │ LED gate, jack-det │                   │ jack-output   │           │
  │   │ → GPIO47 amp       │                   │ overlay       │           │
  │   └─────────┬──────────┘                   │               │           │
  │             │ volume bar / mute / output  ►│               │           │
  │                                            ▲│               │           │
  │   ┌──────────────────┐  set_base_hue ──────┘│               │           │
  │   │ ha_airplay_artwork│                     │               │           │
  │   │ tjpgd in ROM,    │                     └───────────────┘           │
  │   │ persistent decode│                                                  │
  │   │ pool, latest    ◄── audio tap ◄─ playback_task                     │
  │   │ JPEG retained   │                                                  │
  │   └────────┬─────────┘                                                 │
  └────────────┼──────────────────────────────────────────────────────────┘
               │ JPEG bytes
               │
   ┌───────────┴─────────────────────────────────────────────────────────┐
   │ main/rtsp/  SET_PARAMETER   metadata + image/jpeg hooks              │
   └─────────────────────────────────────────────────────────────────────┘

  playback_task pipeline (core 1, prio 7):
    audio_receiver_read → resample → volume → audio_eq_process →
       chime_consume (silence path) → led_audio_feed → audio_tap →
       i²s_channel_write

  Web layer (esp_http_server on port 80):
    /  /logs                  ── embedded HTML in flash
    /api/system/info          ── status snapshot
    /api/now_playing          ── RTSP-cached metadata + artwork etag
    /api/artwork.jpg          ── latest JPEG, ETag-aware
    /api/eq  GET / POST       ── 15 gains, fires eq_events bus
    /api/device/name  POST    ── friendly name → NVS → mDNS slug on reboot
    /api/ota/update  POST     ── PSRAM-buffered, SHA-256 verified
    /api/system/restart  POST
    /ws/logs   (WebSocket)    ── live log stream
```

## Data flow (steady state, music playing)

1. **iPhone → RTSP** → `main/rtsp/` parses frames, decrypts, decodes ALAC.
2. **Decoded PCM** goes through `main/audio/audio_buffer.c` (sorted jitter buffer) into `playback_task`.
3. `playback_task` resamples if needed, applies digital volume, then **before** writing I²S:
   - calls `chime_stop()` (in case a connect chime was still in flight),
   - calls `audio_eq_process()` (15 cascaded peaking biquads, ~620 µs / block),
   - calls `led_audio_feed()` (upstream single-LED indicator, no-op here),
   - calls `ha_airplay_audio_tap()` (our bass-band RMS + beat detector).
4. I²S master output on GPIO7/8/10 → TLV320AIC3204 → 3.5 mm jack. The internal amp on GPIO47 is enabled by default; jack-detect on GPIO17 (200 ms debounced) flips it LOW when a plug is inserted, and the LED ring shows a 5 s cyan/amber overlay.
5. `ha_airplay_leds` render task runs at 50 Hz on core 1. It reads the tap's atomic energy/beat values plus the artwork-derived hue and renders one of seven states (mute / volume / conn-in / conn-out / output-change / playing / idle). The slide on GPIO3 gates only PLAYING + IDLE; the others render unconditionally.
6. RTSP session events (`CLIENT_CONNECTED`, `PLAYING`, `PAUSED`, `DISCONNECTED`, `METADATA`) fan out via `rtsp_events_register()` to `ha_airplay_ui/ui.c` (drives LED state), `now_playing.c` (caches title/artist/album/state for `/api/now_playing`), and the chime trigger in `main/main.c` (plays the MagSafe arpeggio on `CLIENT_CONNECTED`).
7. Artwork `SET_PARAMETER` (`image/jpeg`) is forwarded to `ha_airplay_artwork_update()`. That copies the JPEG into PSRAM and posts to a decoder task; tjpgd decodes at 1/8 scale, sums RGB → mean → HSV, calls `ha_airplay_leds_set_base_hue()`, and retains the JPEG bytes for `/api/artwork.jpg`.
8. `eq_events` bus: a `POST /api/eq` from the dashboard emits `EQ_EVENT_ALL_BANDS_SET`. The `audio_eq` listener recomputes 15 biquad coefficient sets on the calling task, atomically swaps the active index, and persists gains to NVS. The audio task picks up the new coefficients on its next read of `s_active`.

## Concurrency

| Task                       | Core | Priority | Purpose                                                            |
|----------------------------|------|---------:|--------------------------------------------------------------------|
| `audio_play`               | 1    | 7        | Resample + volume + EQ + chime + audio tap + I²S write             |
| `ha_airplay_leds`          | 1    | 3        | 50 Hz pixel render                                                 |
| `ha_airplay_art`           | 0    | 3        | JPEG decode on track change + retain bytes for `/api/artwork.jpg`  |
| `ha_airplay_enc`           | any  | 10       | Quadrature state machine                                           |
| `ha_airplay_btn`           | any  | 8        | Multi-click + long-press                                           |
| `ha_airplay_sw`            | any  | 6        | Slide-switch → decorative-LED gate                                 |
| `ha_airplay_jack`          | any  | 6        | GPIO17 jack-detect → GPIO47 amp toggle + 5 s LED indicator         |
| `ha_airplay_volpoll`       | any  | 4        | 5 Hz NVS-cache poll for iOS slider                                 |
| `wifi_scan`                | 0    | 3        | One-shot scan+connect on STA start                                 |
| `log_ws` (broadcast)       | 0    | 3        | 100 ms WebSocket fan-out for `/ws/logs`                            |
| Upstream RTSP / AirPlay-pair / PTP tasks (see airplay-esp32 README)                                  |

Core 0 runs WiFi and the full AirPlay protocol stack. Core 1 runs the audio playback task; the LED render task shares core 1 but at much lower priority so it cannot starve audio.

## Key design decisions

- **Framework**: pure ESP-IDF v5.4.1 — no ESPHome runtime overhead. Required to fit AirPlay 2 + WiFi + UI within the <100 KB free internal heap gate (Phase 0). See `phase0-report.md`.
- **AirPlay 2 (default)**: upstream's AirPlay 2 stack is what iOS negotiates today. We briefly tried `CONFIG_AIRPLAY_FORCE_V1=y` to get encoder → iPhone slider sync via DACP, but modern iOS no longer sends DACP-ID to non-MFi devices. Flipped back. Device → iPhone sync is an accepted protocol limit.
- **Hardware volume**: AIC3204 digital-volume registers (page 0, regs 65/66). Software scaling before the DAC would cost SNR headroom.
- **Software EQ on the audio task**: 15-band cascaded peaking biquads (DF2T, single-precision float on the LX7 FPU). Sits between volume and the audio tap in `playback_task`. Coefficients are double-buffered with an atomic-int swap so the control plane never blocks the hot path. ~620 µs per 8 ms block (~7.5 % of one core). Codec-agnostic — the AIC3204 has no biquad helpers in our driver, so doing it in software was the cheapest path.
- **Slide switch as decorative-only gate** (since `e66be29`): GPIO45 (LED VCC rail) is held HIGH for the device's lifetime; the slide on GPIO3 toggles a software flag that suppresses only PLAYING and IDLE renders. Utility overlays (mute, volume bar, connection sweep, output-destination) render in either position.
- **Factory-seed-once for WiFi + device name**: the board layer seeds NVS from `wifi_config.h` only when the corresponding NVS key is empty, so runtime renames or credential changes via the dashboard / captive portal stick across reboots. The compile-time header is the factory default, not an enforced override.
- **Web layer embedded in flash, no SPIFFS dependency**: HTML pages and the MagSafe chime PCM ride in via `EMBED_FILES`. Survives any reflash; no need to pre-populate SPIFFS.
- **OTA rollback safety**: the new app is marked `ESP_OTA_IMG_PENDING_VERIFY`. Once the image observes 60 s of healthy network plus AirPlay started, `network_monitor_task` calls `esp_ota_mark_app_valid_cancel_rollback()`. A bad image that boots far enough to start the OTA but later crashes will roll back automatically on the next reboot.
- **AirPlay pairing layer namespace** (`main/airplay_pair/`): the directory was originally `main/hap/` and exported `hap_init` / `hap_session_*` etc. for the AirPlay-2 transient pairing crypto. The naming clashed with the HomeKit Accessory Protocol when we briefly experimented with `esp-homekit-sdk`. The directory was renamed to make the boundary clear; HomeKit experiment was reverted (see CHANGELOG).
- **JPEG decoding in ROM**: ESP32-S3 has tjpgd built into ROM — zero flash cost, ~15 ms per 512×512 artwork at 1/8 scale. The decoded JPEG bytes are also retained in PSRAM so `/api/artwork.jpg` can serve them with a content-derived ETag. See `components/ha_airplay_artwork/README.md`.

## Phase map

| Phase | Status   | Scope                                                          |
|-------|----------|----------------------------------------------------------------|
| 0     | ✅ GREEN | Viability PoC: AIC3204 audio out, AirPlay discover/stream, heap gate. [phase0-report.md](phase0-report.md) |
| 1     | ✅       | Encoder volume, button controls, WS2812B ring with beat-pulse + artwork hue, slide switch → decorative LED gate. [phase1-report.md](phase1-report.md) |
| 2     | 🛠 partial | ✅ jack-detect (GPIO17 → GPIO47, 5 s output-destination LED). 📅 still open: spectrum LED mode, long-press mode cycle, persistent LED mode in NVS. |
| 3     | planned  | OTA polish, boot <8 s, soak test, production artwork cache.    |
| —     | ✅       | **Sprints A–D** (review-driven, see `docs/review/SUMMARY.md`): software 15-band EQ, web dashboard, live log viewer, hostname slug, MagSafe chime, jitter tolerance, OTA rollback safety, RTSP soft-fail, DMAP depth limit, friendly-name UI, factory-seed-once. |
