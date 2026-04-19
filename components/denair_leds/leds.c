/**
 * @file leds.c
 * @brief DenAir 12-LED WS2812B ring engine (Phase 1).
 *
 * Runs a FreeRTOS task on core 1 that renders at 50 Hz. Receives events
 * (volume-change, muted) via a small API; idle pattern is a slow amber
 * breath. Phase 2 will expand this into the four PRD-mandated modes
 * driven from the AirPlay audio buffer.
 */

#include "denair_leds.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include <stdatomic.h>
#include <math.h>
#include <string.h>

#define LED_GPIO         21
#define LED_COUNT        12
#define LED_RENDER_HZ    50
#define LED_RENDER_MS    (1000 / LED_RENDER_HZ)

static const char TAG[] = "denair_leds";

static led_strip_handle_t s_strip = NULL;
static TaskHandle_t s_task = NULL;

typedef enum {
  MODE_IDLE = 0,
  MODE_VOLUME_OVERLAY,
  MODE_MUTED,
} led_mode_t;

static _Atomic int s_mode = MODE_IDLE;
static _Atomic int s_volume_fill_x1000 = 500;   // 0..1000 (fraction * 1000)
static _Atomic int64_t s_volume_overlay_until_us = 0;
static _Atomic bool s_muted = false;

/* ---------- Rendering helpers ---------- */

static void render_idle(int64_t now_us) {
  /* Slow amber breathing: 4 s period, gentle amber ~255/140/20 peak. */
  float t = (float)(now_us / 1000) / 1000.0f; // seconds
  float phase = fmodf(t, 4.0f) / 4.0f;        // 0..1
  float brightness;
  if (phase < 0.5f) {
    brightness = phase * 2.0f;
  } else {
    brightness = (1.0f - phase) * 2.0f;
  }
  uint8_t r = (uint8_t)(40.0f * brightness);
  uint8_t g = (uint8_t)(16.0f * brightness);
  uint8_t b = 0;
  for (int i = 0; i < LED_COUNT; i++) {
    led_strip_set_pixel(s_strip, i, r, g, b);
  }
}

static void render_volume_bar(void) {
  int fill_x1000 = atomic_load(&s_volume_fill_x1000);
  /* How many full LEDs to light (1..12 scale). */
  int lit = (fill_x1000 * LED_COUNT + 500) / 1000;
  if (lit < 0) lit = 0;
  if (lit > LED_COUNT) lit = LED_COUNT;

  for (int i = 0; i < LED_COUNT; i++) {
    if (i < lit) {
      uint8_t r, g, b;
      /* Green → yellow → red gradient across the ring. */
      if (i < 8) {
        r = 0; g = 80; b = 0;
      } else if (i < 10) {
        r = 90; g = 60; b = 0;
      } else {
        r = 120; g = 0; b = 0;
      }
      led_strip_set_pixel(s_strip, i, r, g, b);
    } else {
      led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
  }
}

static void render_muted(void) {
  /* Two dim red LEDs opposite each other (positions 3 and 9 — quarter/
   * three-quarter on a 12 LED ring). Rest off. Matches the reference
   * yaml's muted indicator. */
  for (int i = 0; i < LED_COUNT; i++) {
    if (i == 3 || i == 9) {
      led_strip_set_pixel(s_strip, i, 60, 0, 0);
    } else {
      led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
  }
}

/* ---------- Task ---------- */

static void led_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    int64_t now = esp_timer_get_time();

    /* Pick rendering mode for this frame. Muted beats volume-overlay
     * beats idle. */
    led_mode_t mode;
    if (atomic_load(&s_muted)) {
      mode = MODE_MUTED;
    } else if (now < atomic_load(&s_volume_overlay_until_us)) {
      mode = MODE_VOLUME_OVERLAY;
    } else {
      mode = MODE_IDLE;
    }
    atomic_store(&s_mode, mode);

    switch (mode) {
    case MODE_MUTED:           render_muted();          break;
    case MODE_VOLUME_OVERLAY:  render_volume_bar();     break;
    case MODE_IDLE:
    default:                    render_idle(now);        break;
    }
    led_strip_refresh(s_strip);

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_RENDER_MS));
  }
}

/* ---------- API ---------- */

esp_err_t denair_leds_init(void) {
  if (s_strip) return ESP_OK;

  led_strip_config_t strip_cfg = {
      .strip_gpio_num = LED_GPIO,
      .max_leds = LED_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .mem_block_symbols = 64,
      .flags.with_dma = false,
  };
  esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
    return err;
  }
  led_strip_clear(s_strip);
  led_strip_refresh(s_strip);

  /* Render task on core 1 — leaves core 0 for WiFi/AirPlay. */
  BaseType_t ok = xTaskCreatePinnedToCore(led_task, "denair_leds", 3072, NULL,
                                          3, &s_task, 1);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreatePinnedToCore(denair_leds): out of memory");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "LED ring up (GPIO %d, %d pixels, %d Hz)", LED_GPIO,
           LED_COUNT, LED_RENDER_HZ);
  return ESP_OK;
}

void denair_leds_show_volume(float fraction, int hold_ms) {
  if (!s_strip) return;
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  int fill = (int)(fraction * 1000.0f + 0.5f);
  atomic_store(&s_volume_fill_x1000, fill);
  atomic_store(&s_volume_overlay_until_us,
               esp_timer_get_time() + (int64_t)hold_ms * 1000);
}

void denair_leds_set_muted(bool muted) {
  atomic_store(&s_muted, muted);
}
