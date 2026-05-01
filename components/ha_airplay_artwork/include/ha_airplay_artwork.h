#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Home Assistant AirPlay artwork → LED-hue extractor.
 *
 * AirPlay sends JPEG album artwork (~180 KB typical) via RTSP. We copy
 * the bytes into PSRAM, hand them to a decoder task that runs tjpgd
 * (from the ESP32-S3 ROM — free flash), accumulates mean RGB across
 * the decoded image, converts to HSV, and publishes the dominant hue
 * to ha_airplay_leds as the PLAYING-mode base color.
 */

esp_err_t ha_airplay_artwork_init(void);

/**
 * Queue a newly-received JPEG for analysis.
 * @param bytes  JPEG payload — copied internally; caller keeps ownership
 *               of the original buffer.
 * @param len    payload size in bytes
 * @param content_type  MIME string ("image/jpeg" or "image/png"; PNG is
 *                      currently skipped — hue stays at the last value)
 */
void ha_airplay_artwork_update(const uint8_t *bytes, size_t len,
                           const char *content_type);

/**
 * Borrow the latest JPEG bytes for serving over HTTP. The internal
 * buffer is held under a mutex while the lock is taken; release the
 * lock as soon as the bytes are copied or sent.
 *
 * Usage:
 *   const uint8_t *p; size_t n; uint32_t etag;
 *   if (ha_airplay_artwork_lock(&p, &n, &etag) == ESP_OK) {
 *     // serve bytes (do not retain across releases)
 *     ha_airplay_artwork_unlock();
 *   }
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no artwork has been
 *         received yet, ESP_ERR_TIMEOUT if the lock could not be taken.
 */
esp_err_t ha_airplay_artwork_lock(const uint8_t **out_bytes, size_t *out_len,
                                  uint32_t *out_etag);
void ha_airplay_artwork_unlock(void);

/**
 * Read just the etag of the current artwork without taking the bytes
 * lock. Used for cheap "did the artwork change?" polling.
 * @return current etag, or 0 if no artwork is cached.
 */
uint32_t ha_airplay_artwork_etag(void);
