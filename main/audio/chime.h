#pragma once

/**
 * One-shot chime player. The PCM blob is embedded at build time and
 * pumped through the I²S writer by `playback_task` whenever the receiver
 * has no audio of its own. If a real AirPlay stream starts mid-chime,
 * the chime simply gets cut short on the next frame.
 *
 * Typical use: `chime_play()` from a control-plane callback (e.g. RTSP
 * `CLIENT_CONNECTED`); `chime_consume()` from the audio task.
 */

#include <stddef.h>
#include <stdint.h>

void chime_init(void);

/** Trigger the chime. Restarts from the beginning if already playing. */
void chime_play(void);

/**
 * Stop any chime in progress. Idempotent. Called from the audio task
 * the moment real audio frames start arriving so the chime doesn't
 * resume mid-arpeggio after a silence gap.
 */
void chime_stop(void);

/**
 * Pull up to `stereo_frames` of chime samples into `out`. Returns the
 * number of stereo frames actually written; 0 means no chime is active.
 *
 * Output samples are pre-attenuated by -6 dB so the chime doesn't peak
 * at full digital scale relative to the music that follows. Hardware
 * volume on the codec applies on top.
 */
size_t chime_consume(int16_t *out, size_t stereo_frames);
