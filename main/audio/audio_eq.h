#pragma once

/**
 * Software 15-band parametric EQ for the I²S output path.
 *
 * Sits in `playback_task` between volume scaling and the LED audio tap.
 * Listens to the project-wide `eq_events` bus so the existing web UI and
 * settings layer drive it the same way they drive the TAS58XX hardware EQ.
 *
 * Coefficient compute happens on the listener's calling task; the audio
 * task only ever reads. Updates are published via a double-buffered
 * coefficient table with a single atomic index store, so there is no
 * lock on the hot path.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_EQ_BANDS 15

/**
 * Initialize the EQ engine, register on the eq_events bus, restore any
 * saved gains from NVS, and arm the in-line `audio_eq_process` call.
 *
 * @param sample_rate Output sample rate (Hz) of the I²S writer. Coefficients
 *                    are computed at this rate.
 */
esp_err_t audio_eq_init(uint32_t sample_rate);

/**
 * Recompute coefficients for a new sample rate. Cheap; meant to be called
 * if the I²S output rate ever changes.
 */
esp_err_t audio_eq_set_sample_rate(uint32_t sample_rate);

/**
 * Replace all 15 band gains (dB). Recomputes coefficients on the calling
 * task and atomically swaps them in for the audio task.
 */
esp_err_t audio_eq_set_gains(const float gains_db[AUDIO_EQ_BANDS]);

/** Reset gains to flat (0 dB on every band). */
esp_err_t audio_eq_flat(void);

/** Master enable. When disabled, `audio_eq_process` returns immediately. */
void audio_eq_set_enabled(bool enabled);
bool audio_eq_is_enabled(void);

/**
 * In-place stereo PCM filtering. Called from the playback task at
 * priority 7. Returns immediately if EQ is disabled or every band is
 * within ±0.05 dB of flat.
 *
 * @param pcm           int16 stereo, L/R interleaved
 * @param stereo_frames number of L/R pairs
 */
void audio_eq_process(int16_t *pcm, size_t stereo_frames);

/** Return the band's center frequency in Hz (for log/diagnostics). */
float audio_eq_get_center_freq(int band);
