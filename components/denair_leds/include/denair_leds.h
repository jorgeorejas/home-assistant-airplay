#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * DenAir LED ring engine — 12× WS2812B on GPIO21 (HA Voice PE).
 *
 * Phase 1 scope: volume-bar overlay on encoder turn, amber-breathing idle
 * pattern the rest of the time. Phase 2 will add audio-reactive modes
 * (VU / Beat Pulse / Spectrum) driven from the AirPlay PCM buffer via
 * FFT on core 1.
 */
esp_err_t denair_leds_init(void);

/**
 * Show a volume bar for `hold_ms` then fade back to the idle pattern.
 * @param fraction  0.0 .. 1.0 — fills LEDs 1..12 proportionally
 */
void denair_leds_show_volume(float fraction, int hold_ms);

/**
 * Muted-state indicator: dim red pair until cleared. Set `muted=false`
 * to return to the idle pattern.
 */
void denair_leds_set_muted(bool muted);
