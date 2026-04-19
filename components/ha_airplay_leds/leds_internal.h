#pragma once

#include <stdint.h>

/* Internal glue between audio_tap.c and leds.c — not part of the public
 * ha_airplay_leds.h surface. */

int64_t ha_airplay_audio_last_beat_us(void);
uint32_t ha_airplay_audio_bass_q24(void);
uint32_t ha_airplay_audio_rms_q24(void);
