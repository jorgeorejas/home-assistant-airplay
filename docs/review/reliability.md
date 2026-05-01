# Reliability & robustness review

**Scope:** Read-only audit of phase0 `homeassistant-vivla` (commit `e66be29`, branch `phase0-raop`) targeting long-uptime reliability and the user-reported sub-second audio dropouts.
**Date:** 2026-05-01

## TL;DR
- **OTA is one-shot fatal.** No `esp_ota_mark_app_*` calls anywhere — a bad update bricks the device until manual reflash, even though the partition table presumably supports rollback.
- **`ESP_ERROR_CHECK` panics on transient init failures.** `audio_receiver_init`, `audio_output_init`, `mdns_init`, `rtsp_server_start`, `esp_wifi_start`, `esp_event_handler_instance_register` all panic on first non-OK return — and several of them run *every* time WiFi reassociates after a long outage.
- **WiFi reconnect path has a window where the device is stuck in pure-STA mode with no AP fallback.** Once `IP_EVENT_STA_GOT_IP` fires, AP is torn down (`wifi.c:117`); if the AP later goes away permanently, the device cannot recover the captive portal until reboot.
- **The dropout is almost certainly not the I²S DMA buffer.** With `dma_desc_num=16, dma_frame_num=256` you have ~93 ms of I²S buffering already. The realtime path's `target_buffer_frames` defaults to **200 ms / nominal_frame_samples** (`audio_timing.c:12`, `:78`), and the receiver has 1000-slot capacity (`audio_buffer.h:26`). Under WiFi jitter the bottleneck is the *anchor-late detection logic*, not buffer depth — see the dedicated section below.
- **No mutex on `eq_events` listener list.** It's written from `web_server_start` (HTTPD task) and read from `audio_eq_init` / `chime_init` (main task) and emitted from any task that touches EQ — a UAF on long-uptime if a listener is ever unregistered while another task is mid-emit.

## Findings

### [P0] OTA never marks the new app as valid → bad update bricks the device
**Where:** `main/network/ota.c` (entire file), `main/network/web_server.c:265` (`ota_update_handler`), `main/main.c:210` (`app_main`).
**Failure mode:** A firmware build that boots far enough to call `esp_restart` from `ota_update_handler` (line 286) but later crashes (e.g. faulty I²S init, missing `wifi_config.h` symbol, NULL deref in `audio_receiver_init`) is permanently latched as the boot partition. There is no `esp_ota_mark_app_valid_cancel_rollback()` after first successful boot — `app_main` never calls it. The bootloader will roll back only if `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is set *and* the new image was begun in `ESP_OTA_IMG_NEW` state, which the buffered path (`esp_ota_begin(... fw_size, ...)`, line 106) does correctly mark as pending — but with no `mark_app_valid` ever called, every boot is a "didn't mark as valid" boot, so subsequent reboots will always rollback if rollback is enabled and reset the device to the previous image. Whichever way the config is set, the behaviour is wrong.
**Current behavior:** Either every reboot rolls back to the previous image (if rollback is on), or a bad image is permanently latched (if rollback is off). Neither is correct.
**Suggested fix:** Add `esp_ota_mark_app_valid_cancel_rollback()` near the end of `app_main` after a "device is healthy" event — e.g. once `wifi_is_connected()` returns true *and* `s_airplay_started` is set, or after the network monitor task observes 60 s of network uptime. Verify `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is in the chosen state. Also add `esp_partition_check_identity` validation against the running partition before commit.
**Effort:** S

### [P0] Init panics on transient errors that should be recoverable
**Where:** `main/main.c:71-77` (`hap_init`, `audio_receiver_init`, `audio_output_init`, `audio_eq_init`, `now_playing_init`, all wrapped in `ESP_ERROR_CHECK`); `main/main.c:84` (`rtsp_server_start`); `main/main.c:267` (`audio_realtime_preallocate`); `main/network/mdns_airplay.c:149,156` (`mdns_init`, `mdns_hostname_set`); `main/network/wifi.c:227,228,289-292` (wifi_init/start).
**Failure mode:** `audio_receiver_init` returns `ESP_ERR_NO_MEM` on PSRAM exhaustion — panic. `mdns_init` returns `ESP_FAIL` if the netif is in a transient state during a WiFi flap — panic. `rtsp_server_start` failure (e.g. socket exhaustion after BT/WiFi co-ex disturbs lwIP) — panic. The infrastructure init only runs once (gated by `s_airplay_infrastructure_ready`), but `rtsp_server_start` runs on *every* network-up edge in `network_monitor_task`, so a transient `ESP_FAIL` on hour 73 of uptime reboots the box.
**Current behavior:** Reboot loop on transient resource exhaustion. iOS clients see the receiver disappear repeatedly.
**Suggested fix:** For `rtsp_server_start` in `start_airplay_services`, replace `ESP_ERROR_CHECK` with `if (err != ESP_OK) { ESP_LOGE(...); s_airplay_started = false; return; }` so the next network-monitor tick retries. For `mdns_init`, treat `ESP_ERR_INVALID_STATE` (already initialized) as ok like `wifi_init_base` does for `esp_netif_init`. For `audio_receiver_init`/`audio_output_init`, log and refuse to mark `s_airplay_infrastructure_ready=true` so the next tick retries instead of panicking. Audio is the *primary* function — failing soft and retrying beats a panic-restart cascade.
**Effort:** M

### [P1] WiFi: AP mode is permanently torn down once STA gets an IP
**Where:** `main/network/wifi.c:114-119` (in `IP_EVENT_STA_GOT_IP` handler: `esp_wifi_set_mode(WIFI_MODE_STA)`).
**Failure mode:** Day 1: device joins SSID `Foo`, AP shuts down, mDNS works, all good. Day 8: SSID `Foo` is renamed or its WPA password changes. The device disconnects, retries 5 times → `enable_ap_mode()` is called (`wifi.c:102`) which sets back to APSTA — *but only if* `esp_wifi_get_mode()` returns something other than APSTA. That check works. However: `enable_ap_mode` re-runs `esp_netif_create_default_wifi_ap()` only if `s_ap_netif` is null; on this code path it's already non-null from the original init, so `esp_netif_create_default_wifi_ap` is *skipped*, and we reuse the old netif which was deleted by the implicit teardown when mode was switched to STA-only at `wifi.c:118`. Result depends on IDF internals — at minimum log warning, possibly a NULL netif handle in lwIP.
**Current behavior:** Hard to reason about; very unlikely to be exercised in QA.
**Suggested fix:** Don't tear AP down in the `GOT_IP` handler at all. Keep APSTA for the lifetime of the device. The only cost is one extra SSID in the user's WiFi list and ~20 KB of lwIP state. The robustness win — captive portal always reachable, even after credential changes — is worth it.
**Effort:** S

### [P1] Anchor-late detection causes silence cascades under WiFi jitter
**Where:** `main/audio/audio_timing.c:30-31` (`MAX_CONSECUTIVE_LATE 3`), `:22` (`BULK_FLUSH_LATE_THRESHOLD_US 2000000`), `:294` (`stats->buffer_underruns++`).
**Failure mode:** A WiFi packet burst delays an RTP packet by 30 ms. The late-frame detector trips on 3 consecutive late frames (~24 ms). What happens next depends on the bulk-flush threshold — frames more than 2 s late trigger a full flush; frames less than that are individually drained. Either way the user hears a sub-second silence and the consumer state thrashes. The 3-frame threshold is described as "just enough to distinguish a genuine stale-buffer from a one-off WiFi jitter spike" — but a one-off jitter spike on a contended 2.4 GHz network *easily* spans 3 frames @ 8 ms = 24 ms.
**Current behavior:** Brief WiFi delays produce audible drops.
**Suggested fix:** Bump `MAX_CONSECUTIVE_LATE` to 12-16 (~100-130 ms, well below the 200 ms `DEFAULT_BUFFER_LATENCY_US`). Also instrument `stats->buffer_underruns` in periodic log output and the `/system/info` JSON so you can correlate user-reported dropouts with this counter. See the dedicated audio-dropouts section.
**Effort:** S

### [P1] No `esp_ota_mark_app_valid_cancel_rollback` is the most-likely-to-bite-you bug
*(See [P0] above — re-listed because it's the #1 thing to fix.)*

### [P2] `eq_events` listener list has no mutex
**Where:** `main/audio/eq_events.c:13-46`.
**Failure mode:** `eq_events_register` is called from `audio_eq_init` (main task) at boot. `eq_events_emit` is called from the HTTPD task (`web_server.c:589`) and could plausibly be called from the audio path if a future EQ-changed-mid-stream feature lands. Without atomic_int or a mutex around `s_listener_count`, an interleaved register+emit can read a half-written `callback` pointer and crash. Today this is benign because registration happens once at boot before HTTPD is up, but the file structure invites future bugs.
**Current behavior:** Race window is tiny but real on multi-core S3.
**Suggested fix:** Either (a) add a comment + assert that registration must complete before any task other than main calls emit, or (b) wrap the array in a `portMUX_TYPE` critical section. Option (b) is 8 lines.
**Effort:** S

### [P2] `now_playing_get` returns zeroed metadata on 20 ms timeout
**Where:** `main/now_playing.c:89-95`.
**Failure mode:** A web-server request to `/api/now-playing` during heavy RTSP traffic times out on the mutex (20 ms is generous but not infinite when the metadata write loop is also running). The handler returns a *zeroed* `now_playing_t`, blanking the UI rather than returning the previous-known state.
**Current behavior:** UI flickers to "nothing playing" intermittently while music is in fact playing.
**Suggested fix:** On timeout, leave `*out` untouched (caller passes its own buffer to be filled — a no-op timeout means caller sees the last successful read, or zero if uninitialized). Or use a copy-snapshot pattern: a second `now_playing_t` updated by the writer and atomically swap-pointers — no mutex on the read path.
**Effort:** S

### [P2] `chime.c` mixes atomic and non-atomic state
**Where:** `main/audio/chime.c:26-67` (`s_pos` non-atomic, `s_active` atomic).
**Failure mode:** `chime_play()` writes `s_active=false; s_pos=0; s_active=true` (lines 46-48). The audio task in `chime_consume` reads `s_active` (acquire), then reads `s_pos` (relaxed). If the audio task observes `s_active==true` *but* observes a stale `s_pos` (e.g. partway through a previous playback), it plays back a fragment of the wrong segment. ESP32-S3 with cache coherency makes this rare but not impossible across cores. Audible glitch is small (≤20 ms at the start of a chime).
**Current behavior:** Possible micro-glitch at start of chime; rarely noticed.
**Suggested fix:** Either make `s_pos` `_Atomic size_t` and use `memory_order_release` after writing 0, or place a `__sync_synchronize()` between the `s_pos = 0` and the `atomic_store(s_active, true)`. The `release` on the atomic alone does not order the prior non-atomic write of `s_pos` portably, though on Xtensa it usually does.
**Effort:** S

### [P2] Multiple `gpio_install_isr_service` calls
**Where:** `components/ha_airplay_ui/button.c:132`, `encoder.c:92`, `led_switch.c:78`. Each calls `gpio_install_isr_service(0)` and tolerates `ESP_ERR_INVALID_STATE`, which is correct, but means the *first* one to run silently determines whether the service exists. If `ha_airplay_button_start` runs after `encoder_start` in some refactor, no problem. But the call order is implicit in `ha_airplay_ui_init`. Also: the ISR allocates default-attribute (non-IRAM) — passing flag `0` is fine for handlers, but the GPIO ISR dispatcher itself runs from flash, which means cache-disabled events (e.g. SPI flash writes during NVS commit during OTA) miss interrupts. Encoder will glitch during OTA writes; chime/playback is unaffected because audio runs from PSRAM.
**Current behavior:** Encoder ticks may be lost during NVS writes (settings persist) or OTA flash writes — both rare, but real.
**Suggested fix:** Pass `ESP_INTR_FLAG_IRAM` to `gpio_install_isr_service`, since all three ISRs are already `IRAM_ATTR`. Centralize the call to a single boot hook.
**Effort:** S

### [P3] Unbounded `event_client_socket` lifetime; no per-session cleanup on TCP RST
**Where:** `main/rtsp/rtsp_handlers.c:182-225` (event_port_task).
**Failure mode:** When `recv(MSG_PEEK)` returns 0 (clean shutdown), the socket is closed cleanly. But on TCP RST or network blackhole, `recv` blocks until the platform's TCP keepalive expires (LWIP default: minutes to hours). During that window `event_client_socket` is non-negative, the next `accept()` close-and-reuse path runs — which is fine — but `RTSP_EVENT_DISCONNECTED` is *not* emitted from the event task (only `RTSP_EVENT_CLIENT_CONNECTED` is). The disconnect is only emitted from the main RTSP server. If the iPhone hard-kills (airplane mode) the event-port socket lingers per LWIP keepalive defaults.
**Current behavior:** mDNS/HAP state may stay "client connected" for several minutes after iPhone vanishes.
**Suggested fix:** Set `SO_KEEPALIVE` + a short `TCP_KEEPIDLE`/`TCP_KEEPINTVL` (e.g. 30 s / 10 s) on `event_client_socket` after accept, and on the realtime listening socket. Emit `RTSP_EVENT_DISCONNECTED` from the event task when its peer drops.
**Effort:** S

### [P3] OTA streaming fallback path has no SHA validation
**Where:** `main/network/ota.c:144-195` (`ota_streaming`).
**Failure mode:** When PSRAM allocation for buffered OTA fails (`ota.c:201`), the streaming path runs unvalidated. A truncated upload (client TCP reset) at byte 99% is detected by `recv_len <= 0` (line 168) which calls `esp_ota_abort` — good. But a *malformed* image (right size, wrong contents) passes `esp_ota_end`'s integrity check only if the appended SHA covers what was sent; otherwise it boots a corrupt image. Combined with [P0], this is a brick.
**Current behavior:** Streaming OTA path is a fallback that's lower-trust than the buffered path, but treated identically post-end.
**Suggested fix:** Either remove the streaming fallback entirely (require PSRAM for OTA) or compute SHA-256 incrementally during streaming and compare against the appended digest before `esp_ota_end`.
**Effort:** M

## On the audio-dropout question specifically

**Most likely root cause: anchor-late detection thrashing, not buffer depth.**

Evidence:
- The DMA queue is now 16×256 ≈ 93 ms (`audio_output.c:139-140`). That's ample for any non-pathological WiFi event.
- The realtime ring buffer is 1000 slots × ~352 samples = ~8 s capacity at 44.1 kHz (`audio_buffer.h:26`). Almost never the bottleneck.
- The receiver target buffer is 200 ms (`DEFAULT_BUFFER_LATENCY_US`, `audio_timing.c:12`).
- The dropout-causing constant is `MAX_CONSECUTIVE_LATE = 3` (`audio_timing.c:31`), at ~8 ms/frame ≈ 24 ms. **A 24 ms WiFi jitter spike is unremarkable on 2.4 GHz**, especially with a phone moving around the room or microwave/Bluetooth interference.

When 3 consecutive frames cross the 40 ms-late threshold (`TIMING_THRESHOLD_US`), the timing layer drains. If they're more than 2 s late it does the bulk flush; if they're 50-200 ms late it individually drops them and the audio task gets `samples=0` from `audio_receiver_read`, which falls to the silence path in `audio_output.c:118-122`. The user hears a sub-second silence.

**What to instrument to confirm:**
1. Add `stats->buffer_underruns` and a new `stats->late_drains` counter to `/api/now-playing` JSON so you can poll during a drop.
2. Add a periodic log (every 10 s) of `audio_buffer_get_frame_count()` — if it's hovering at 50-200 frames during smooth playback but momentarily drops to 0 around dropout time, jitter is the cause. If it's always 800+ then jitter isn't the issue and the dropout is downstream (DAC underrun / I²S DMA stall).
3. Log `consecutive_late_frames` when it crosses 1 — every dropout will have it cross 3 just before silence.

**Single-line mitigation:** raise `MAX_CONSECUTIVE_LATE` from 3 to 16. At 8 ms/frame that's 128 ms — comfortably below the 200 ms pre-buffer, but tolerant of normal WiFi jitter. This won't make a *broken* anchor scenario worse (the bulk-flush threshold still catches multi-second drift) but will hide ordinary jitter.

A complementary win: lower I²S `dma_frame_num` from 256 to 64 and instead increase `dma_desc_num` to 32 (same total ms but more granular underrun reporting — which currently never logs anything because the queue is 4× too coarse to observe sub-frame underrun events).

## Healthy
- `audio_buffer.c` slot-pool design (sorted index + free stack) is clean, properly locked with `portMUX_TYPE`, and handles overflow with semaphore re-sync (`audio_buffer.c:62`). Good.
- `audio_eq.c` double-buffered coefficients with `atomic_int s_active` is textbook-correct lock-free publication. No issues.
- `ha_airplay_artwork.c` queue with drop-old policy and PSRAM-backed copies (`artwork.c:213-220`) cannot grow unbounded. Good.
- `audio_realtime_preallocate` (`main.c:267`) is a smart anti-fragmentation move — pre-allocating before WiFi/TLS is exactly the right pattern.
