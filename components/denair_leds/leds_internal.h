#pragma once

#include <stdint.h>

/* Internal glue between audio_tap.c and leds.c — not part of the public
 * denair_leds.h surface. */

int64_t denair_audio_last_beat_us(void);
uint32_t denair_audio_bass_q24(void);
uint32_t denair_audio_rms_q24(void);
