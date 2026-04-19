# `ha_airplay_artwork`

Decodes AirPlay album artwork on-device and publishes the dominant hue to the LED ring as its PLAYING-mode base colour.

## Flow

```
  AirPlay RTSP SET_PARAMETER (image/jpeg, ~180 KB)
                │
                ▼
  main/rtsp/rtsp_handlers.c:~1440
  ha_airplay_artwork_update(bytes, len, content_type)
                │     copies to PSRAM
                ▼
  xQueueSend → artwork_task (core 0, prio 3)
                │
                ▼
  jd_prepare + jd_decomp (tjpgd from ESP32-S3 ROM)
     scale = 1/8, output = RGB888 rectangles
                │     sum R/G/B across all rectangles
                ▼
  mean RGB → HSV → if (S>0.18 && V>0.12) vivid=1
                │
                ▼
  ha_airplay_leds_set_base_hue(hue_deg, vivid)
```

## Why tjpgd from ROM

The ESP32-S3's mask ROM ships tjpgd (ChaN's Tiny JPEG Decompressor). That means:

- **Zero flash cost.** The decoder is already resident.
- **Trivial dependencies.** `#include "esp32s3/rom/tjpgd.h"`, link against `esp_rom`, done.
- **Plenty fast.** At 1/8 scale a 512×512 artwork (the iPhone's typical size) decodes in ~15 ms on idle core 0.

The tradeoff is that tjpgd is basic — no progressive JPEG, no exotic subsampling modes, no CMYK. For album artwork that's not a real constraint; Apple Music and Spotify both ship baseline JPEGs.

## Memory profile

- **PSRAM buffer** per pending artwork: `len` bytes (typically 100-300 KB). Capped at 256 KB in `ARTWORK_MAX_BYTES`.
- **Internal-heap decode pool**: 3.2 KB (`TJPGD_POOL_BYTES`) — enough for the decoder's working memory with `JD_USE_SCALE + JD_TBLCLIP`.
- **Queue**: 2 slots (`ARTWORK_QUEUE_LEN`). If a third artwork arrives before the decoder catches up (very unlikely — track changes are seconds apart, decode is milliseconds), the oldest pending job is discarded and its buffer freed.
- **Task stack**: 4 KB.

No state is retained across decodes beyond the published hue; a new track wholly supersedes the old one.

## Dominant-hue algorithm

Simple mean, not histogram-mode or k-means:

```
for each decoded rectangle:
    r_sum += sum of R bytes
    g_sum += sum of G bytes
    b_sum += sum of B bytes
    px_count += w × h
mean_rgb = (r_sum, g_sum, b_sum) / px_count
hsv = rgb_to_hsv(mean_rgb / 255)
```

Why mean works for album art: covers are often dominated by one colour region (the band photo, a tinted background, etc.), so the mean lands close to it. Cases where mean fails (two equally-weighted contrasting blobs, busy collages) fall into the **vivid gate**:

```
vivid = (saturation > 0.18) && (value > 0.12)
```

If the mean lands near grey (both channels roughly equal) or near black (low value), `ha_airplay_leds_set_base_hue` is called with `enabled = false` and the ring falls back to time-rotation. This stops us from painting the ring a muddy desaturated almost-white that nobody asked for.

A histogram-based dominant-hue extractor would be more accurate at the cost of a larger output buffer and more math — available in `tests/colour/` upstream on request, not shipped in Phase 1.

## Public API (`include/ha_airplay_artwork.h`)

```c
esp_err_t ha_airplay_artwork_init(void);

void ha_airplay_artwork_update(const uint8_t *bytes, size_t len,
                           const char *content_type);
```

- `init` creates the queue + task. Called once from `main/main.c` after UI init.
- `update` is called by `rtsp_handlers.c` every time an `image/jpeg` arrives. `content_type == "image/png"` is silently skipped — PNG decoding would need a separate library and album art is almost universally JPEG.

## Example log line

```
I (58595) ha_airplay_artwork: decoding 512×512 JPEG (scale 1/8, 180224 B)
I (58611) ha_airplay_artwork: artwork mean RGB=(0.21,0.07,0.04) → HSV=(18°,0.81,0.21) vivid=1
```

— artwork for RATA's *Dejarse los nudillos*. Dominant warm red (hue 18°), heavily saturated, dim overall (value 0.21 because the cover is mostly a dark band photo). Vivid gate passes; ring base settles to that red.
