# Performance & resource review

**Scope:** Audit of the AirPlay receiver's audio pipeline, LED/artwork tasks, memory layout, and binary/boot characteristics on ESP32-S3R8 (HA Voice PE). Read-only audit; no source modified.
**Date:** 2026-05-01

## TL;DR
- **EQ hot path is not a real concern.** Worst-case ~600 µs / 8 ms block (~7.5% of one core); fixed-point or IRAM placement is premature. The actual risk is a *flash i-cache miss* on first call after a long pause, not steady-state throughput.
- **Internal heap is the binding resource and we are over-spending it.** Realtime RX stacks are pinned to MALLOC_CAP_INTERNAL (~24 KB+ / `audio_stream_realtime.c:49–54`); JPEG decoder allocates a 3.2 KB internal-heap pool *per artwork* mid-stream (`artwork.c:110`). Both are avoidable.
- **DMA buffer is overprovisioned.** 16×256 = 93 ms pre-roll buys ~50 ms of jitter resilience over 8×256 (46 ms) but spends 16 KB internal SRAM and 50 ms of play/pause latency. 8×512 (≈93 ms) is a strict win.
- **LED render pinned to core 1 alongside the audio task at prio 3.** Render time is ≥1.5 ms (RMT clocked at 10 MHz × 12 LEDs × 24 bits) plus float HSV math. Risk of 50 Hz wakeup landing inside an EQ block on core 1.
- **PSRAM is 95% empty during streaming.** 5.4 MB free; the 1 MB sorted ring is the only large consumer. Latitude exists for larger artwork queue, log ring, or pre-buffer tuning.

## Findings

### [HIGH] Mid-stream `MALLOC_CAP_INTERNAL` allocation in JPEG path
**Where:** `components/ha_airplay_artwork/artwork.c:110`
**Issue:** `heap_caps_malloc(3200, MALLOC_CAP_INTERNAL)` runs every time the iPhone pushes new artwork — multiple times per session, sometimes mid-track. The pool is freed immediately after decode, but the malloc/free cycle fragments internal heap exactly when the largest contiguous block (was 73 KB at gate time — `phase0-report.md:41`) matters most. tjpgd does not require IRAM.
**Suggested fix:** Allocate the 3200 B pool once at `ha_airplay_artwork_init` and reuse across decodes. Or move it to PSRAM (`MALLOC_CAP_SPIRAM`) — tjpgd accesses it sequentially, PSRAM hit is a single-digit-µs latency add on a 15 ms decode budget. Persistent allocation is the simplest.
**Expected payoff:** Removes a recurrent ~3 KB internal-heap churn event; protects the largest-free-block metric. Saves ~5 µs of malloc per decode.
**Effort:** S

### [HIGH] DMA depth: 8×512 dominates 16×256
**Where:** `main/audio/audio_output.c:139–140`
**Issue:** Both deliver ≈93 ms buffered, but i2s_std DMA descriptors carry per-descriptor overhead (handler invocation per descriptor). 16 descriptors → callback ~5.8 ms; 8 descriptors → callback ~11.6 ms. Either way, the playback task wakes well inside the 100 ms cushion, so descriptor count is over-engineered. Larger `dma_frame_num` reduces callback-rate overhead and ISR pressure.
**Suggested fix:** Try `dma_desc_num=8, dma_frame_num=512`. Same total buffer, half the ISR rate, ~50% fewer descriptor-table cache lines. Validate with a 30-min soak that dropouts haven't reappeared.
**Expected payoff:** ~halved DMA-completion ISR rate (less context noise); identical perceived latency. Internal SRAM unchanged.
**Effort:** S

### [HIGH] Realtime RX stacks held in internal RAM
**Where:** `main/audio/audio_stream_realtime.c:49–54`
**Issue:** Two task stacks (`AUDIO_RECV_STACK_SIZE`, `AUDIO_CTRL_STACK_SIZE`) are forced into `MALLOC_CAP_INTERNAL`. These tasks do `recvfrom` on UDP sockets; lwIP can run with PSRAM stacks on S3 if the descriptor write barrier is honoured (xtensa has cache coherency for shared state — IDF docs allow it for non-DMA users). Combined with the typical 4–8 KB per stack this is plausibly ~10–20 KB of internal-heap pressure that PSRAM could absorb.
**Suggested fix:** Verify whether moving these stacks to `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` causes any failure (ISR-dispatch tasks must stay internal, but a normal recv worker shouldn't). Hypothesis to validate: the original choice was conservative inheritance from upstream, not a measured requirement.
**Expected payoff:** Recover 10–20 KB of internal heap → moves the 168 KB free metric closer to 200 KB and grows the largest-free-block past 90 KB. **Mark as hypothesis — needs benchmarking.**
**Effort:** M

### [MEDIUM] EQ hot path: float is fine, but i-cache miss risk
**Where:** `main/audio/audio_eq.c:202–251`
**Issue:** Estimated worst-case for `audio_eq_process` at 352 stereo frames × 15 bands × 2 ch × ~9 FP ops ≈ 95 k ops. ESP32-S3 LX7 single-precision FPU pipelines at ~1 cycle for FMUL/FMADD when register-resident. Realistic budget with int↔float conversions and load-stores: ~1.6 cyc/op × 240 MHz ≈ **620 µs per block** (block period 7.98 ms ≈ 7.8% of one core). Headroom is comfortable. **However**, the function and its `biquad_df2t` are in flash; first call after the audio_play task has been preempted long enough for cache lines to evict will incur i-cache fills (~20–60 µs spike). Float context-switch cost is real (~80 B FPU state per switch) but only once per pause→resume.
**Suggested fix:** No change to fixed-point — not warranted. Mark `audio_eq_process` and `biquad_df2t` with `IRAM_ATTR` *only if* you observe periodic glitches that correlate with cache pressure. Cheaper first step: profile with `esp_timer_get_time` deltas across the call to confirm steady-state numbers and capture the tail.
**Expected payoff:** Mostly bounds worst-case latency; steady-state delta is small. IRAM cost ≈1–1.5 KB of the ~32 KB IRAM budget.
**Effort:** S (instrument); M (move to IRAM if needed)

### [MEDIUM] LED render task on core 1 at prio 3, alongside audio at prio 7
**Where:** `components/ha_airplay_leds/leds.c:259, 304`; `audio_output.c:184`
**Issue:** Both pinned to core 1. RMT transmit for 12 WS2812 pixels at 10 MHz is ~360 µs of bit-banging, plus ~50 µs of float HSV math. Render every 20 ms (50 Hz). Audio task at prio 7 will preempt it cleanly, so audio is safe — but the LED task spending 50 µs in `expf`/`fmodf` is wasted work since the result barely changes between frames. More important: a 50 Hz `vTaskDelayUntil` aligns deterministically with audio block boundaries (every 7.98 ms), so the *same* relative timing repeats. Worth confirming there's no resonance with the I²S callback cadence.
**Suggested fix:** Drop LED render to 30 Hz (33 ms period) — visually indistinguishable on a 12-pixel ring, 40% fewer wakeups. Cache `hsv_to_rgb` results when hue and brightness move <1°/<1%. Optional: pin LED task to core 0 to remove the same-core contention story entirely (it does not interact with anything on core 1).
**Expected payoff:** ~1–2% CPU on core 1; cleaner audio-task time budget.
**Effort:** S

### [MEDIUM] Boot-time DAC ramp dominates startup budget
**Where:** `components/dac_tlv320aic3204/dac_tlv320aic3204.c:160`
**Issue:** `vTaskDelay(pdMS_TO_TICKS(2500))` for HP soft-step is ESPHome-parity but *blocks* `iot_board_init` → `app_main` cannot proceed past the board init line. PRD §5.7 target: <8 s. Phase 0 evidence shows AirPlay-ready at boot+17.7 s (`phase0-report.md:34`) — the 2.5 s codec ramp is ~14% of that. Wifi link to first IP is the rest.
**Suggested fix:** Move the DAC init to a one-shot task that runs in parallel with WiFi association. AirPlay services don't need the DAC ready until the first audio frame arrives. Audio output would gate on a "DAC ready" event.
**Expected payoff:** Trims ~2 s off boot if WiFi association ≥2.5 s (typical).
**Effort:** M

### [MEDIUM] Now-playing mutex / metadata write path
**Where:** `main/now_playing.c:16, 89`
**Issue:** Mutex held for ~5 strncpy calls + struct copy = ~1 µs typical. Writers are RTSP handler tasks; readers are web-server tasks. This is fine. The 20 ms timeout is generous; an audio-path reader (which there isn't, but if any future feature reads metadata from `playback_task`) would block too long. Not a hot-path concern.
**Suggested fix:** None required. If anything ever reads from the audio task, replace with seqlock or atomic-double-buffer (same pattern `audio_eq.c` uses).
**Expected payoff:** Defensive — no current cost.
**Effort:** S

### [LOW] EQ recompute from web context invalidates audio filter state
**Where:** `main/audio/audio_eq.c:178`
**Issue:** `audio_eq_set_gains` resets `s_state[][]` to zero from the web/event task — concurrent with `audio_eq_process` reading those state cells from the audio task. Race is benign (a transient zero in a biquad memory cell is inaudible) but technically a data race. The coefficient swap is properly atomic; the state reset is not.
**Suggested fix:** Skip the memset; let the new coefficients ring out the old state for ~2 ms. Or have the audio task drain and reset on a flag. Lowest priority.
**Expected payoff:** Strict-aliasing/TSAN cleanliness; no audible change.
**Effort:** S

### [LOW] Binary trajectory
**Where:** `phase0-report.md:18` (1.4 MB), current 1.6 MB / 3 MB partition
**Issue:** Two phases added 200 KB. At that rate ~7 more phase-equivalents before the partition fills. The 186 KB chime PCM blob is the single largest line item; HTML pages are ~14 KB combined. Risk-trajectory is manageable but the chime is a fat target.
**Suggested fix:** If pressure rises, swap chime to 22.05 kHz mono ALAW or short Opus → 30–50 KB. Until then, no action needed.
**Expected payoff:** ~140 KB reclaimed if/when chime is recompressed.
**Effort:** M (depends on Opus availability in current build).

### [LOW] PSRAM under-utilised
**Where:** Cross-cutting; `audio_buffer.h:26` (`MAX_RING_BUFFER_FRAMES 1000`)
**Issue:** Sorted-ring pool already in PSRAM (1000 × ~1428 B = 1.4 MB). PSRAM has 4 MB+ free at steady state. We could grow `MAX_RING_BUFFER_FRAMES` for higher-jitter networks, or grow the artwork queue from 2 to e.g. 4 to drop fewer mid-track artwork pushes. No urgency; worth knowing the slack exists.
**Expected payoff:** Resilience optionality; no current pain point.
**Effort:** S

## Measurements worth taking
- **EQ hot-path timing.** Wrap `audio_eq_process` in `esp_timer_get_time()` deltas, log p50/p99/max over a 5-min window. Confirms the 600 µs steady-state estimate and quantifies the post-pause spike.
- **Internal-heap watermark during sustained stream + artwork pushes.** `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` sampled per second for 30 min — establishes whether the artwork-decode pool actually erodes the largest-free-block metric.
- **DMA callback ISR rate.** Either via `esp_timer` deltas in a custom callback or perfmon — validates the 8×512 vs 16×256 hypothesis with a number, not a guess.
- **Per-task CPU%.** Enable `CONFIG_FREERTOS_USE_TRACE_FACILITY` + `vTaskGetRunTimeStats`; dump every 10 s. Tells us whether `audio_play`, `ha_airplay_leds`, lwIP, and `tcpip_thread` add up to the headroom we think they do.
- **Boot timeline (10 ms granularity).** Sprinkle `esp_timer_get_time()` markers from app_main entry through `AirPlay ready`; identifies whether the 2.5 s DAC ramp or WiFi association dominates.

## Healthy
- **Audio buffer pool in PSRAM with internal-RAM index arrays** (`audio_buffer.c:126–145`) is exactly the right split — large pool out of internal heap, hot index/sort array fast.
- **Atomic double-buffered EQ coefficient publish** (`audio_eq.c:35–88`) is a clean lock-free pattern; the audio task never blocks the control plane.
- **WiFi `WIFI_PS_NONE`** (`wifi.c:228`) and PSRAM @ 80 MHz already set. Standard knobs are dialled in the right direction.
- **Pre-allocation of audio task stacks before WiFi/TLS init** (`main.c:267`) is a deliberate fragmentation defense and matches the "internal heap is tight" reality.
