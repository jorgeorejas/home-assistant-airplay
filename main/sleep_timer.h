#pragma once

/**
 * Sleep timer — fades audio out and mutes the device after a
 * user-chosen interval. Designed for the "fall asleep with music"
 * scenario.
 *
 * Behavior:
 *   - Last 60 s: linear digital-volume fade from current_db down to
 *     -30 dB.
 *   - At T=0: `dac_set_power_mode(STANDBY)` so the codec is silent
 *     even if a stream is still flowing.
 *   - The pre-fade volume is captured on start and restored on cancel
 *     (or on the user's next encoder turn, which clears mute by
 *     existing UI logic).
 *
 * Single global timer; calling `start` while already armed reschedules.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t sleep_timer_init(void);

/**
 * Arm the timer for `minutes`. Pass 0 to cancel an active timer (and
 * restore the pre-timer volume if the fade hadn't finished yet).
 */
esp_err_t sleep_timer_set(int minutes);

typedef struct {
  bool active;
  int minutes_remaining; /* rounded up to whole minutes */
  bool fading;            /* in the last 60 s fade-out window */
} sleep_timer_state_t;

void sleep_timer_get(sleep_timer_state_t *out);
