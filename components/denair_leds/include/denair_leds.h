#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * DenAir LED ring engine — 12× WS2812B on GPIO21 (HA Voice PE).
 *
 * State machine (highest priority first):
 *   1. MUTE — two dim red pixels until cleared.
 *   2. VOLUME — 2 s horizontal bar overlay after the user (or iOS) changes
 *      the volume.
 *   3. CONNECTION — 5 s white rotating sweep when a new AirPlay client
 *      connects; 2 s red fade-out when one disconnects.
 *   4. PLAYING — audio-reactive: dim slowly-rotating base hue with a
 *      whole-ring flash on each detected beat, 400 ms exponential decay.
 *   5. IDLE — 4 s amber breathing.
 */

typedef enum {
  DENAIR_PLAYBACK_IDLE = 0,
  DENAIR_PLAYBACK_CONNECTED,   /* client connected but not streaming */
  DENAIR_PLAYBACK_PLAYING,     /* audio actively flowing */
  DENAIR_PLAYBACK_PAUSED,      /* client connected, stream paused */
  DENAIR_PLAYBACK_DISCONNECTED /* transient — flashes then returns to IDLE */
} denair_playback_state_t;

esp_err_t denair_leds_init(void);

/**
 * Master LED power. Drives GPIO45 (the WS2812B VCC rail) and gates the
 * render-task's strip refresh. Used by the mute-slide-switch repurpose
 * on HA Voice PE: flipping the physical slide → turning the ring off.
 */
void denair_leds_set_power(bool on);
bool denair_leds_is_powered(void);

/** Set the base playback state that drives the PLAYING / IDLE visuals. */
void denair_leds_set_playback_state(denair_playback_state_t state);

/** Show a volume bar for `hold_ms` then fade back to the state above.
 *  @param fraction 0.0..1.0 */
void denair_leds_show_volume(float fraction, int hold_ms);

/** 5 s connection overlay (white rotating sweep). */
void denair_leds_flash_connection(void);

/** Muted indicator overrides everything else until cleared. */
void denair_leds_set_muted(bool muted);

/**
 * Override the PLAYING-mode base hue with a value derived from album
 * artwork. When enabled=false (e.g. the image is near-grey) the engine
 * falls back to the time-based hue rotation.
 *
 * @param hue_deg  0..360, dominant hue
 * @param enabled  true to use it, false to revert to auto-rotate
 */
void denair_leds_set_base_hue(float hue_deg, bool enabled);

/**
 * Audio tap — called from the audio playback task right before each
 * I2S write. Computes bass-band energy and drives a simple beat
 * detector. All heavy lifting stays on the audio core; the render task
 * only reads published atomics.
 *
 * @param samples          16-bit signed stereo interleaved, L R L R ...
 * @param stereo_frames    number of stereo frames (i.e. samples array
 *                         length / 2)
 */
void denair_audio_tap(const int16_t *samples, size_t stereo_frames);
