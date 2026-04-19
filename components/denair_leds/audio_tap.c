/**
 * @file audio_tap.c
 * @brief Cheap bass-band energy + beat detector fed from the audio_output
 *        playback task. Publishes values atomically for the LED render
 *        loop to consume.
 *
 * Design:
 *   - Downmix to mono, apply a single-pole low-pass at ~150 Hz to isolate
 *     bass energy (everything below is in the kick/bass range).
 *   - Accumulate |x|² across the frame to get band RMS².
 *   - Maintain an exponential moving average of band energy over ~1.5 s.
 *   - Beat fires when instantaneous energy exceeds avg × threshold AND at
 *     least BEAT_MIN_GAP_MS have passed since the last one. The LED loop
 *     decides how to render.
 *   - Everything runs in the audio_output task's context; no FFT, no
 *     queues, no extra task. ~200 arithmetic ops per stereo frame.
 */

#include "denair_leds.h"

#include "esp_timer.h"

#include <math.h>
#include <stdatomic.h>

/* Low-pass coefficient: one-pole IIR y[n] = y[n-1] + a * (x[n] - y[n-1])
 * a ≈ 1 - exp(-2π·fc/fs). For fc=150 Hz at 48 kHz this is ~0.0195. Close
 * enough at 44.1 kHz too (~0.0213). Hard-coded constant — Phase 1 does not
 * retune per sample rate; the difference is inaudible on the LED side. */
#define LPF_ALPHA_Q15 640   /* 0.0195 × 32768 */

/* Beat detector */
#define AVG_ALPHA_Q15  328  /* ~0.01, ~1.6 s EMA time constant at 100 fps */
#define BEAT_THRESHOLD 140  /* instantaneous > avg × 1.40 fires a beat */
#define BEAT_MIN_GAP_MS 250 /* lockout — no beats faster than 240 BPM */

/* Atomic outputs the LED loop reads */
static _Atomic uint32_t s_bass_energy_q24 = 0; /* normalized 0..1 × 2^24 */
static _Atomic uint32_t s_rms_q24 = 0;
static _Atomic int64_t  s_last_beat_us = 0;

/* State carried across frames — touched only from the audio task */
static int32_t s_lp_state = 0;       /* 32-bit LPF memory */
static uint64_t s_avg_energy = 0;    /* scaled-up EMA of frame energy */

static inline int32_t sat_i32_from_u64_sqrt(uint64_t x) {
  /* Rough scale-to-24-bit for LED consumption. int16 samples, stereo,
   * 352-sample frames at 0 dBFS → peak frame |x|² ~ 352 × 32767² ≈ 3.78e11.
   * sqrt is ~6.1e5. Divide by 16 to land in the ~0..2^16 range that the
   * render code expects; clamp into 24 bits for headroom. */
  if (x == 0) return 0;
  double root = sqrt((double)x);
  int32_t out = (int32_t)(root / 16.0);
  if (out < 0) out = 0;
  if (out > (1 << 24)) out = 1 << 24;
  return out;
}

void denair_audio_tap(const int16_t *samples, size_t stereo_frames) {
  if (!samples || stereo_frames == 0) return;

  int32_t lp = s_lp_state;
  uint64_t frame_sq = 0;
  uint64_t bass_sq = 0;

  for (size_t i = 0; i < stereo_frames; i++) {
    int32_t L = samples[2 * i];
    int32_t R = samples[2 * i + 1];
    int32_t mono = (L + R) >> 1;    /* int16 + int16 fits in int32 */

    frame_sq += (uint64_t)(mono * mono);

    /* Single-pole low-pass in Q15 coefficient form */
    int64_t delta = (int64_t)mono - lp;
    lp += (int32_t)((delta * LPF_ALPHA_Q15) >> 15);

    int64_t b64 = lp;
    bass_sq += (uint64_t)(b64 * b64);
  }
  s_lp_state = lp;

  /* EMA of bass energy — scale bass_sq down first to avoid overflow over
   * long sessions, then apply Q15 EMA coefficient. */
  uint64_t inst = bass_sq / (stereo_frames > 0 ? stereo_frames : 1);
  uint64_t diff = (inst > s_avg_energy) ? (inst - s_avg_energy)
                                        : (s_avg_energy - inst);
  uint64_t step = (diff * AVG_ALPHA_Q15) >> 15;
  s_avg_energy = (inst > s_avg_energy) ? (s_avg_energy + step)
                                       : (s_avg_energy - step);

  /* Beat detection: instantaneous > avg × (BEAT_THRESHOLD / 100) */
  bool fired_beat = false;
  if (s_avg_energy > 0) {
    uint64_t threshold = (s_avg_energy * BEAT_THRESHOLD) / 100;
    int64_t now_us = esp_timer_get_time();
    int64_t gap_us = now_us - atomic_load(&s_last_beat_us);
    if (inst > threshold && gap_us >= (int64_t)BEAT_MIN_GAP_MS * 1000) {
      atomic_store(&s_last_beat_us, now_us);
      fired_beat = true;
    }
  }
  (void)fired_beat;

  /* Publish energy levels for the LED loop. */
  uint64_t rms_inst = frame_sq / (stereo_frames > 0 ? stereo_frames : 1);
  atomic_store(&s_rms_q24, (uint32_t)sat_i32_from_u64_sqrt(rms_inst));
  atomic_store(&s_bass_energy_q24, (uint32_t)sat_i32_from_u64_sqrt(inst));
}

/* Accessors used by leds.c — exposed via internal header. */
int64_t denair_audio_last_beat_us(void) { return atomic_load(&s_last_beat_us); }
uint32_t denair_audio_bass_q24(void)    { return atomic_load(&s_bass_energy_q24); }
uint32_t denair_audio_rms_q24(void)     { return atomic_load(&s_rms_q24); }
