#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Home Assistant AirPlay LED ring engine — 12× WS2812B on GPIO21 (HA Voice PE).
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
  HA_AIRPLAY_PLAYBACK_IDLE = 0,
  HA_AIRPLAY_PLAYBACK_CONNECTED,   /* client connected but not streaming */
  HA_AIRPLAY_PLAYBACK_PLAYING,     /* audio actively flowing */
  HA_AIRPLAY_PLAYBACK_PAUSED,      /* client connected, stream paused */
  HA_AIRPLAY_PLAYBACK_DISCONNECTED /* transient — flashes then returns to IDLE */
} ha_airplay_playback_state_t;

esp_err_t ha_airplay_leds_init(void);

/**
 * Decorative-renders gate. When disabled, the render task skips PLAYING
 * and IDLE animations and writes blank pixels instead — but utility
 * overlays (MUTE, VOLUME, CONN_IN, CONN_OUT) still render normally so
 * the user keeps essential feedback. Used by the slide-switch on Voice
 * PE: flipping the physical slide → silence the ambient/idle effects
 * without losing the volume dial.
 *
 * The WS2812B VCC rail (GPIO45) is held HIGH for the lifetime of the
 * device after init; the rail is no longer power-gated by this flag.
 */
void ha_airplay_leds_set_decorative_enabled(bool enabled);
bool ha_airplay_leds_decorative_is_enabled(void);

/** Set the base playback state that drives the PLAYING / IDLE visuals. */
void ha_airplay_leds_set_playback_state(ha_airplay_playback_state_t state);

/** Show a volume bar for `hold_ms` then fade back to the state above.
 *  @param fraction 0.0..1.0 */
void ha_airplay_leds_show_volume(float fraction, int hold_ms);

/** 5 s connection overlay (white rotating sweep). */
void ha_airplay_leds_flash_connection(void);

/** Muted indicator overrides everything else until cleared. */
void ha_airplay_leds_set_muted(bool muted);

/**
 * 5 s output-destination overlay shown when the 3.5 mm jack is inserted
 * or removed. Cyan ring = audio routed to jack; amber ring = audio
 * routed to internal speaker. Renders in the utility-overlay slot so it
 * shows even with the slide in decorative-off position.
 *
 * @param jack_in true if the jack just got plugged in, false on removal
 */
void ha_airplay_leds_show_output_change(bool jack_in);

/**
 * Override the PLAYING-mode base hue with a value derived from album
 * artwork. When enabled=false (e.g. the image is near-grey) the engine
 * falls back to the time-based hue rotation.
 *
 * @param hue_deg  0..360, dominant hue
 * @param enabled  true to use it, false to revert to auto-rotate
 */
void ha_airplay_leds_set_base_hue(float hue_deg, bool enabled);

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
void ha_airplay_audio_tap(const int16_t *samples, size_t stereo_frames);
