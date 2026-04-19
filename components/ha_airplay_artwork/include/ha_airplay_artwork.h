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
