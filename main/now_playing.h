#pragma once

/**
 * Caches the latest AirPlay metadata + transport state so the web layer
 * can serve `/api/now_playing` without coupling to the RTSP event bus.
 *
 * Subscribes to the RTSP event bus once at boot. Reads from the web
 * task; writes from the RTSP listener — protected by a small mutex.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NOW_PLAYING_IDLE = 0,
  NOW_PLAYING_CONNECTED,
  NOW_PLAYING_PLAYING,
  NOW_PLAYING_PAUSED,
} now_playing_state_t;

typedef struct {
  now_playing_state_t state;
  char title[64];
  char artist[64];
  char album[64];
  char genre[64];
  uint32_t duration_secs;
  uint32_t position_secs;
  bool has_artwork;
} now_playing_t;

esp_err_t now_playing_init(void);

/** Snapshot the cache. Thread-safe; copies into the caller's struct. */
void now_playing_get(now_playing_t *out);
