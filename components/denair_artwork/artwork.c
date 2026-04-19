/**
 * @file artwork.c
 * @brief Extract a dominant hue from AirPlay JPEG artwork and push it
 *        to the LED ring as the PLAYING-mode base color.
 *
 * Runs entirely on core 0 off a queued task so the RTSP handler returns
 * immediately. Uses tjpgd from ROM (ESP32-S3 has it built-in) with a
 * 1/8 scale — 512×512 artwork decodes to 64×64 = 4096 pixels, which is
 * plenty for a mean-color estimate and takes ~15 ms on an idle core.
 */

#include "denair_artwork.h"

#include "denair_leds.h"

#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char TAG[] = "denair_artwork";

#define TJPGD_POOL_BYTES   3200     /* plenty for JD_USE_SCALE + JD_TBLCLIP */
#define ARTWORK_QUEUE_LEN  2
#define ARTWORK_MAX_BYTES  (256 * 1024)  /* sanity cap — any real artwork fits */
#define DECODE_SCALE       3        /* 1/8 — decoder emits 64×64 for 512×512 in */

typedef struct {
  uint8_t *bytes;   /* PSRAM-allocated copy */
  size_t   len;
} artwork_job_t;

typedef struct {
  const uint8_t *bytes;
  size_t         offset;
  size_t         total;
  uint64_t       r_sum;
  uint64_t       g_sum;
  uint64_t       b_sum;
  uint64_t       px_count;
} ctx_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;

/* ---------- tjpgd callbacks ---------- */

static UINT tjd_input(JDEC *jd, BYTE *buf, UINT nbyte) {
  ctx_t *c = (ctx_t *)jd->device;
  size_t remaining = c->total - c->offset;
  size_t n = nbyte > remaining ? remaining : nbyte;
  if (buf) {
    memcpy(buf, c->bytes + c->offset, n);
  }
  c->offset += n;
  return (UINT)n;
}

static UINT tjd_output(JDEC *jd, void *bitmap, JRECT *rect) {
  ctx_t *c = (ctx_t *)jd->device;
  const uint8_t *pix = (const uint8_t *)bitmap;
  UINT w = rect->right - rect->left + 1;
  UINT h = rect->bottom - rect->top + 1;
  UINT count = w * h;
  uint64_t r = 0, g = 0, b = 0;
  for (UINT i = 0; i < count; i++) {
    r += pix[3 * i + 0];
    g += pix[3 * i + 1];
    b += pix[3 * i + 2];
  }
  c->r_sum += r;
  c->g_sum += g;
  c->b_sum += b;
  c->px_count += count;
  return 1; /* continue */
}

/* ---------- RGB → HSV ---------- */

static void rgb_to_hsv(float r, float g, float b,
                       float *h, float *s, float *v) {
  float maxv = fmaxf(r, fmaxf(g, b));
  float minv = fminf(r, fminf(g, b));
  float delta = maxv - minv;

  *v = maxv;
  *s = (maxv > 0.0f) ? delta / maxv : 0.0f;

  if (delta < 1e-6f) {
    *h = 0.0f;
    return;
  }
  float hue;
  if (maxv == r)       hue = 60.0f * fmodf((g - b) / delta, 6.0f);
  else if (maxv == g)  hue = 60.0f * (((b - r) / delta) + 2.0f);
  else                 hue = 60.0f * (((r - g) / delta) + 4.0f);
  if (hue < 0.0f) hue += 360.0f;
  *h = hue;
}

/* ---------- Decode one job ---------- */

static bool decode_and_publish(uint8_t *bytes, size_t len) {
  void *pool = heap_caps_malloc(TJPGD_POOL_BYTES, MALLOC_CAP_INTERNAL);
  if (!pool) {
    ESP_LOGW(TAG, "decode pool alloc failed");
    return false;
  }

  ctx_t c = { .bytes = bytes, .offset = 0, .total = len };
  JDEC jdec;
  JRESULT err = jd_prepare(&jdec, tjd_input, pool, TJPGD_POOL_BYTES, &c);
  if (err != JDR_OK) {
    ESP_LOGW(TAG, "jd_prepare failed: %d", err);
    free(pool);
    return false;
  }

  ESP_LOGI(TAG, "decoding %u×%u JPEG (scale 1/%d, %u B)",
           jdec.width, jdec.height, 1 << DECODE_SCALE, (unsigned)len);

  err = jd_decomp(&jdec, tjd_output, DECODE_SCALE);
  free(pool);
  if (err != JDR_OK) {
    ESP_LOGW(TAG, "jd_decomp failed: %d", err);
    return false;
  }

  if (c.px_count == 0) return false;

  float rf = (float)c.r_sum / (float)c.px_count / 255.0f;
  float gf = (float)c.g_sum / (float)c.px_count / 255.0f;
  float bf = (float)c.b_sum / (float)c.px_count / 255.0f;

  float h, s, v;
  rgb_to_hsv(rf, gf, bf, &h, &s, &v);

  /* If the mean is near-grey, the image doesn't have a dominant hue —
   * fall back to the time-based rotation rather than drawing muddy
   * off-white. */
  bool vivid = s > 0.18f && v > 0.12f;

  ESP_LOGI(TAG, "artwork mean RGB=(%.2f,%.2f,%.2f) → HSV=(%.0f°,%.2f,%.2f) vivid=%d",
           rf, gf, bf, h, s, v, vivid);

  denair_leds_set_base_hue(h, vivid);
  return true;
}

/* ---------- Task ---------- */

static void artwork_task(void *arg) {
  (void)arg;
  for (;;) {
    artwork_job_t job;
    if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
      if (job.bytes && job.len > 0) {
        (void)decode_and_publish(job.bytes, job.len);
        free(job.bytes);
      }
    }
  }
}

/* ---------- API ---------- */

esp_err_t denair_artwork_init(void) {
  if (s_queue) return ESP_OK;
  s_queue = xQueueCreate(ARTWORK_QUEUE_LEN, sizeof(artwork_job_t));
  if (!s_queue) return ESP_ERR_NO_MEM;
  BaseType_t ok = xTaskCreatePinnedToCore(artwork_task, "denair_art", 4096,
                                          NULL, 3, &s_task, 0);
  if (ok != pdPASS) {
    vQueueDelete(s_queue);
    s_queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "artwork decoder ready");
  return ESP_OK;
}

void denair_artwork_update(const uint8_t *bytes, size_t len,
                           const char *content_type) {
  if (!s_queue || !bytes || len == 0) return;
  if (len > ARTWORK_MAX_BYTES) {
    ESP_LOGW(TAG, "artwork too large: %u B", (unsigned)len);
    return;
  }
  if (content_type && !strstr(content_type, "image/jpeg")) {
    /* PNG support would need a separate decoder — skip for now. */
    ESP_LOGD(TAG, "skipping non-JPEG artwork (%s)", content_type);
    return;
  }

  /* Copy to PSRAM so we don't block the RTSP handler on the downstream
   * decode. The task frees this. */
  uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!copy) {
    ESP_LOGW(TAG, "PSRAM alloc failed (%u B), skipping artwork", (unsigned)len);
    return;
  }
  memcpy(copy, bytes, len);

  artwork_job_t job = { .bytes = copy, .len = len };
  /* Drop old jobs rather than block — a new track supersedes anything
   * still sitting in the queue. */
  if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
    artwork_job_t stale;
    if (xQueueReceive(s_queue, &stale, 0) == pdTRUE && stale.bytes) {
      free(stale.bytes);
    }
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
      free(copy);
    }
  }
}
