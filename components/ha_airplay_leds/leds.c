/**
 * @file leds.c
 * @brief Home Assistant AirPlay 12-LED WS2812B ring engine.
 *
 * Renders at 50 Hz on core 1. State machine:
 *   MUTE (priority 1)  two dim red pixels.
 *   VOLUME (prio 2)    horizontal bar overlay, 2 s hold.
 *   CONN_IN (prio 3)   5 s white rotating sweep on new AirPlay client.
 *   CONN_OUT (prio 3)  2 s dim red fade-out on disconnect.
 *   PLAYING (prio 4)   rotating base hue + whole-ring flash on each beat
 *                      detected by ha_airplay_audio_tap.
 *   IDLE (prio 5)      slow amber breath.
 */

#include "ha_airplay_leds.h"
#include "leds_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define LED_GPIO         21
#define LED_POWER_GPIO   45
#define LED_COUNT        12
#define LED_RENDER_HZ    50
#define LED_RENDER_MS    (1000 / LED_RENDER_HZ)
/* After enabling the VCC rail, wait this long for the LDO to stabilize
 * before we start clocking WS2812 data. Too short → first frame looks
 * garbled until the cap charges. */
#define LED_POWER_SETTLE_MS 15

#define CONN_IN_MS       5000
#define CONN_OUT_MS      2000
#define BEAT_DECAY_MS    400     /* exp decay time constant */

static const char TAG[] = "ha_airplay_leds";

static led_strip_handle_t s_strip = NULL;
static TaskHandle_t s_task = NULL;

/* Atomic state shared between API setters and render task */
static _Atomic int s_playback_state = HA_AIRPLAY_PLAYBACK_IDLE;
static _Atomic int s_volume_fill_x1000 = 500;
static _Atomic int64_t s_volume_overlay_until_us = 0;
static _Atomic int64_t s_conn_in_until_us = 0;
static _Atomic int64_t s_conn_out_until_us = 0;
static _Atomic int64_t s_output_overlay_until_us = 0;
static _Atomic bool s_output_jack_in = false;
static _Atomic int64_t s_track_overlay_until_us = 0;
static _Atomic bool s_muted = false;

/* GPIO45 (WS2812B VCC rail) is driven HIGH at init and stays on. The
   slide-switch now toggles `s_decorative_enabled` instead — utility
   overlays (mute, volume, connection) always render, decorative
   animations (PLAYING, IDLE) blank out when the slide is off. */
static _Atomic bool s_decorative_enabled = true;
static _Atomic int64_t s_power_on_at_us = 0;

/* Track last observed beat timestamp to detect "new beat since last frame" */
static int64_t s_last_rendered_beat_us = 0;
static int64_t s_last_beat_flash_us = 0;

/* Artwork-derived base hue. When s_base_hue_from_art is false the engine
 * uses a time-based hue rotation instead. */
static _Atomic int32_t s_base_hue_centideg = 0;   /* 0..36000 */
static _Atomic bool    s_base_hue_from_art = false;

/* ---------- Pixel helpers ---------- */

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
  float hh = fmodf(h, 360.0f);
  if (hh < 0) hh += 360.0f;
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(hh / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float rf = 0, gf = 0, bf = 0;
  if (hh < 60)       { rf = c; gf = x; bf = 0; }
  else if (hh < 120) { rf = x; gf = c; bf = 0; }
  else if (hh < 180) { rf = 0; gf = c; bf = x; }
  else if (hh < 240) { rf = 0; gf = x; bf = c; }
  else if (hh < 300) { rf = x; gf = 0; bf = c; }
  else               { rf = c; gf = 0; bf = x; }
  *r = (uint8_t)((rf + m) * 255.0f);
  *g = (uint8_t)((gf + m) * 255.0f);
  *b = (uint8_t)((bf + m) * 255.0f);
}

/* ---------- Render modes ---------- */

static void render_idle(int64_t now_us) {
  float t_s = (float)(now_us / 1000) / 1000.0f;
  float phase = fmodf(t_s, 4.0f) / 4.0f;
  float bright = (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;
  uint8_t r = (uint8_t)(40.0f * bright);
  uint8_t g = (uint8_t)(16.0f * bright);
  for (int i = 0; i < LED_COUNT; i++) {
    led_strip_set_pixel(s_strip, i, r, g, 0);
  }
}

static void render_muted(void) {
  for (int i = 0; i < LED_COUNT; i++) {
    if (i == 3 || i == 9) {
      led_strip_set_pixel(s_strip, i, 60, 0, 0);
    } else {
      led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
  }
}

static void render_track_change(int64_t now_us, int64_t until_us) {
  /* 1.5 s rotating shimmer in the artwork hue. Two pixels travel around
     the ring (~1 revolution / 0.5 s) on top of a dim base in the same
     hue. Soft fade-in/out so the transition between artwork hues feels
     deliberate. */
  float remaining_s = (float)(until_us - now_us) / 1e6f;
  float total_s = 1.5f;
  float elapsed_s = total_s - remaining_s;
  float fade_in  = elapsed_s < 0.2f ? elapsed_s / 0.2f : 1.0f;
  float fade_out = remaining_s < 0.4f ? remaining_s / 0.4f : 1.0f;
  float a = fade_in < fade_out ? fade_in : fade_out;
  if (a < 0) a = 0;

  float hue = (float)atomic_load(&s_base_hue_centideg) / 100.0f;
  uint8_t br, bg, bb, hr, hg, hb;
  hsv_to_rgb(hue, 1.0f, 0.18f * a, &br, &bg, &bb);  /* dim base */
  hsv_to_rgb(hue, 0.7f, 0.85f * a, &hr, &hg, &hb);  /* lead pixel */

  int lead = ((int)(elapsed_s * LED_COUNT * 2.0f)) % LED_COUNT;
  if (lead < 0) lead += LED_COUNT;

  for (int i = 0; i < LED_COUNT; i++) {
    int dist = (i - lead + LED_COUNT) % LED_COUNT;
    if (dist == 0 || dist == 1) {
      led_strip_set_pixel(s_strip, i, hr, hg, hb);
    } else {
      led_strip_set_pixel(s_strip, i, br, bg, bb);
    }
  }
}

static void render_output_change(int64_t now_us, int64_t until_us) {
  /* Solid ring in cyan (jack inserted) or amber (internal speaker), with
     a soft fade-in over the first 200 ms and fade-out over the last
     500 ms so the transition feels intentional rather than abrupt. */
  bool jack = atomic_load(&s_output_jack_in);
  float remaining_s = (float)(until_us - now_us) / 1e6f;
  float total_s = 5.0f;
  float elapsed_s = total_s - remaining_s;
  float fade_in = elapsed_s < 0.2f ? elapsed_s / 0.2f : 1.0f;
  float fade_out = remaining_s < 0.5f ? remaining_s / 0.5f : 1.0f;
  float a = fade_in < fade_out ? fade_in : fade_out;
  if (a < 0.0f) a = 0.0f;

  uint8_t r, g, b;
  if (jack) { /* cyan ~= 180° HSV */
    r = (uint8_t)(0   * a);
    g = (uint8_t)(110 * a);
    b = (uint8_t)(120 * a);
  } else {    /* amber ~= 35° HSV */
    r = (uint8_t)(140 * a);
    g = (uint8_t)(60  * a);
    b = (uint8_t)(0);
  }
  for (int i = 0; i < LED_COUNT; i++) {
    led_strip_set_pixel(s_strip, i, r, g, b);
  }
}

static void render_volume_bar(void) {
  int fill = atomic_load(&s_volume_fill_x1000);
  int lit = (fill * LED_COUNT + 500) / 1000;
  if (lit < 0) lit = 0;
  if (lit > LED_COUNT) lit = LED_COUNT;
  for (int i = 0; i < LED_COUNT; i++) {
    if (i < lit) {
      uint8_t r, g, b;
      if (i < 8)      { r = 0;   g = 80; b = 0; }
      else if (i < 10){ r = 90;  g = 60; b = 0; }
      else            { r = 120; g = 0;  b = 0; }
      led_strip_set_pixel(s_strip, i, r, g, b);
    } else {
      led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
  }
}

static void render_conn_in(int64_t now_us, int64_t until_us) {
  /* White pixel chasing around the ring, 1 revolution / second. Everything
   * else dim white. Fade everything out over the last 500 ms. */
  float elapsed_s = (float)((now_us - (until_us - (int64_t)CONN_IN_MS * 1000)) / 1000) / 1000.0f;
  float total_s = (float)CONN_IN_MS / 1000.0f;
  float remaining_s = (float)(until_us - now_us) / 1e6f;
  float fade = (remaining_s < 0.5f) ? remaining_s / 0.5f : 1.0f;
  if (fade < 0) fade = 0;
  (void)elapsed_s; (void)total_s;

  int lead = ((int)(elapsed_s * LED_COUNT)) % LED_COUNT;
  if (lead < 0) lead += LED_COUNT;

  for (int i = 0; i < LED_COUNT; i++) {
    int dist = (i - lead + LED_COUNT) % LED_COUNT;
    float b;
    if (dist == 0)       b = 1.0f;
    else if (dist == 1)  b = 0.5f;
    else if (dist == 2)  b = 0.2f;
    else                 b = 0.05f;
    b *= fade;
    uint8_t v = (uint8_t)(b * 140.0f);
    led_strip_set_pixel(s_strip, i, v, v, v);
  }
}

static void render_conn_out(int64_t now_us, int64_t until_us) {
  float remaining_s = (float)(until_us - now_us) / 1e6f;
  if (remaining_s < 0) remaining_s = 0;
  float fade = remaining_s / ((float)CONN_OUT_MS / 1000.0f);
  uint8_t r = (uint8_t)(fade * 80.0f);
  for (int i = 0; i < LED_COUNT; i++) {
    led_strip_set_pixel(s_strip, i, r, 0, 0);
  }
}

static void render_playing(int64_t now_us) {
  float hue;
  if (atomic_load(&s_base_hue_from_art)) {
    hue = (float)atomic_load(&s_base_hue_centideg) / 100.0f;
  } else {
    /* Fall back to time-based rotation: 1 revolution / 20 s. */
    float t_s = (float)(now_us / 1000) / 1000.0f;
    hue = fmodf(t_s * (360.0f / 20.0f), 360.0f);
  }

  /* Beat flash: when audio_tap fires a new beat, start an exponential
   * decay. Decay factor decays toward 0 with time constant BEAT_DECAY_MS. */
  int64_t last_beat_us = ha_airplay_audio_last_beat_us();
  if (last_beat_us > s_last_rendered_beat_us) {
    s_last_rendered_beat_us = last_beat_us;
    s_last_beat_flash_us = last_beat_us;
  }
  float flash = 0.0f;
  if (s_last_beat_flash_us > 0) {
    float age_ms = (float)(now_us - s_last_beat_flash_us) / 1000.0f;
    flash = expf(-age_ms / (float)BEAT_DECAY_MS);
    if (flash < 0.02f) flash = 0.0f;
  }

  /* Slight RMS-driven breathing on the base brightness so quiet passages
   * don't look static. */
  uint32_t rms_q24 = ha_airplay_audio_rms_q24();
  float rms_norm = (float)rms_q24 / (float)(1 << 20); /* 0..16, clamp */
  if (rms_norm > 1.0f) rms_norm = 1.0f;
  float base_bright = 0.15f + 0.20f * rms_norm;

  uint8_t br, bg, bb;
  hsv_to_rgb(hue, 1.0f, base_bright, &br, &bg, &bb);

  /* Flash adds white on top, scaled by the decay envelope. */
  uint8_t flash_v = (uint8_t)(flash * 180.0f);

  for (int i = 0; i < LED_COUNT; i++) {
    int r = br + flash_v;
    int g = bg + flash_v;
    int b = bb + flash_v;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    led_strip_set_pixel(s_strip, i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
  }
}

/* ---------- Render task ---------- */

static void led_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    int64_t now = esp_timer_get_time();

    /* One-time settle after init powers GPIO45 HIGH — gives the LDO and
     * bypass cap a few ms before we clock the first pixels, otherwise the
     * leading bytes come out garbled. */
    int64_t power_on_at = atomic_load(&s_power_on_at_us);
    if (power_on_at > 0 && now - power_on_at < (int64_t)LED_POWER_SETTLE_MS * 1000) {
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_RENDER_MS));
      continue;
    }

    /* Utility overlays render unconditionally — the user always wants to
     * see mute / volume / connection feedback regardless of the slide. */
    if (atomic_load(&s_muted)) {
      render_muted();
    } else if (now < atomic_load(&s_volume_overlay_until_us)) {
      render_volume_bar();
    } else if (now < atomic_load(&s_conn_in_until_us)) {
      render_conn_in(now, atomic_load(&s_conn_in_until_us));
    } else if (now < atomic_load(&s_conn_out_until_us)) {
      render_conn_out(now, atomic_load(&s_conn_out_until_us));
    } else if (now < atomic_load(&s_output_overlay_until_us)) {
      render_output_change(now, atomic_load(&s_output_overlay_until_us));
    } else if (!atomic_load(&s_decorative_enabled)) {
      /* Slide is off — keep the ring dark when no utility event is active. */
      for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, 0, 0, 0);
      }
    } else if (now < atomic_load(&s_track_overlay_until_us)) {
      render_track_change(now, atomic_load(&s_track_overlay_until_us));
    } else {
      int state = atomic_load(&s_playback_state);
      if (state == HA_AIRPLAY_PLAYBACK_PLAYING) {
        render_playing(now);
      } else {
        render_idle(now);
      }
    }

    led_strip_refresh(s_strip);
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_RENDER_MS));
  }
}

/* ---------- API ---------- */

esp_err_t ha_airplay_leds_init(void) {
  if (s_strip) return ESP_OK;

  /* GPIO45 gates the WS2812B VCC rail. We power it HIGH at init and
   * leave it on so utility overlays (mute, volume, connection) can
   * render any time. The slide switch now toggles a software flag that
   * gates only the decorative renders — see s_decorative_enabled. */
  gpio_config_t power_cfg = {
      .pin_bit_mask = (1ULL << LED_POWER_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&power_cfg);
  gpio_set_level(LED_POWER_GPIO, 1);
  atomic_store(&s_power_on_at_us, esp_timer_get_time());

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

  BaseType_t ok = xTaskCreatePinnedToCore(led_task, "ha_airplay_leds", 4096, NULL,
                                          3, &s_task, 1);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG, "LED ring up (GPIO %d, %d pixels, %d Hz)", LED_GPIO,
           LED_COUNT, LED_RENDER_HZ);
  return ESP_OK;
}

void ha_airplay_leds_set_playback_state(ha_airplay_playback_state_t state) {
  int prev = atomic_exchange(&s_playback_state,
                             (state == HA_AIRPLAY_PLAYBACK_DISCONNECTED
                                  ? HA_AIRPLAY_PLAYBACK_IDLE
                                  : state));
  (void)prev;
  if (state == HA_AIRPLAY_PLAYBACK_DISCONNECTED) {
    atomic_store(&s_conn_out_until_us,
                 esp_timer_get_time() + (int64_t)CONN_OUT_MS * 1000);
  }
}

void ha_airplay_leds_show_volume(float fraction, int hold_ms) {
  if (!s_strip) return;
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  atomic_store(&s_volume_fill_x1000, (int)(fraction * 1000.0f + 0.5f));
  atomic_store(&s_volume_overlay_until_us,
               esp_timer_get_time() + (int64_t)hold_ms * 1000);
}

void ha_airplay_leds_flash_connection(void) {
  if (!s_strip) return;
  atomic_store(&s_conn_in_until_us,
               esp_timer_get_time() + (int64_t)CONN_IN_MS * 1000);
}

void ha_airplay_leds_set_muted(bool muted) {
  atomic_store(&s_muted, muted);
}

void ha_airplay_leds_show_output_change(bool jack_in) {
  if (!s_strip) return;
  atomic_store(&s_output_jack_in, jack_in);
  atomic_store(&s_output_overlay_until_us,
               esp_timer_get_time() + 5LL * 1000 * 1000);
  ESP_LOGI(TAG, "output change: %s", jack_in ? "jack" : "internal speaker");
}

void ha_airplay_leds_show_track_change(void) {
  if (!s_strip) return;
  atomic_store(&s_track_overlay_until_us,
               esp_timer_get_time() + 1500LL * 1000);
}

void ha_airplay_leds_set_decorative_enabled(bool enabled) {
  bool prev = atomic_exchange(&s_decorative_enabled, enabled);
  if (prev == enabled) return;
  ESP_LOGI(TAG, "decorative effects %s", enabled ? "ON" : "OFF");
}

bool ha_airplay_leds_decorative_is_enabled(void) {
  return atomic_load(&s_decorative_enabled);
}

void ha_airplay_leds_set_base_hue(float hue_deg, bool enabled) {
  if (hue_deg < 0.0f) hue_deg += 360.0f;
  if (hue_deg >= 360.0f) hue_deg = fmodf(hue_deg, 360.0f);
  atomic_store(&s_base_hue_centideg, (int32_t)(hue_deg * 100.0f));
  atomic_store(&s_base_hue_from_art, enabled);
}
