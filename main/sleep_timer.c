#include "sleep_timer.h"

#include "dac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

#include <math.h>

static const char *TAG = "sleep_timer";

#define FADE_DURATION_US (60LL * 1000 * 1000) /* last 60 s fade-out */
#define FADE_TICK_MS     500                  /* update every 500 ms */
#define VOL_DB_MUTE      (-30.0f)

static SemaphoreHandle_t s_mutex = NULL;
static esp_timer_handle_t s_fire_timer = NULL;  /* one-shot, fires at end */
static esp_timer_handle_t s_fade_timer = NULL;  /* periodic, runs in last 60 s */

static int64_t s_armed_at_us = 0;
static int64_t s_fire_at_us = 0;
static float s_pre_timer_db = -10.0f;
static bool s_active = false;
static bool s_fading = false;

static void cancel_locked(bool restore_volume) {
  if (s_fire_timer) {
    esp_timer_stop(s_fire_timer);
  }
  if (s_fade_timer) {
    esp_timer_stop(s_fade_timer);
  }
  if (restore_volume && s_active && s_fading) {
    /* User canceled mid-fade — restore the pre-fade volume so they're
       not stuck at a lower level. */
    dac_set_volume(s_pre_timer_db);
    settings_set_volume(s_pre_timer_db);
  }
  s_active = false;
  s_fading = false;
  s_armed_at_us = 0;
  s_fire_at_us = 0;
}

static void on_fade_tick(void *arg) {
  (void)arg;
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  int64_t now = esp_timer_get_time();
  int64_t remaining_us = s_fire_at_us - now;
  if (remaining_us <= 0 || !s_active) {
    xSemaphoreGive(s_mutex);
    return;
  }
  if (remaining_us < FADE_DURATION_US) {
    s_fading = true;
    float frac = (float)remaining_us / (float)FADE_DURATION_US; /* 1 → 0 */
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float target_db = VOL_DB_MUTE + (s_pre_timer_db - VOL_DB_MUTE) * frac;
    dac_set_volume(target_db);
  }

  xSemaphoreGive(s_mutex);
}

static void on_fire(void *arg) {
  (void)arg;
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  ESP_LOGI(TAG, "sleep timer fired — muting");
  if (s_fade_timer) {
    esp_timer_stop(s_fade_timer);
  }
  dac_set_volume(VOL_DB_MUTE);
  dac_set_power_mode(DAC_POWER_STANDBY);
  /* Persist the pre-timer volume so the next encoder turn / unmute
     restores it (the standard mute path uses the saved-volume value). */
  settings_set_volume(s_pre_timer_db);
  s_active = false;
  s_fading = false;

  xSemaphoreGive(s_mutex);
}

esp_err_t sleep_timer_init(void) {
  if (s_mutex) return ESP_OK;
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) return ESP_ERR_NO_MEM;

  esp_timer_create_args_t fire_args = {
      .callback = on_fire, .name = "sleep_fire",
  };
  esp_err_t err = esp_timer_create(&fire_args, &s_fire_timer);
  if (err != ESP_OK) return err;

  esp_timer_create_args_t fade_args = {
      .callback = on_fade_tick, .name = "sleep_fade",
  };
  err = esp_timer_create(&fade_args, &s_fade_timer);
  if (err != ESP_OK) return err;

  ESP_LOGI(TAG, "ready");
  return ESP_OK;
}

esp_err_t sleep_timer_set(int minutes) {
  if (!s_mutex) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  cancel_locked(/*restore_volume=*/true);

  if (minutes <= 0) {
    ESP_LOGI(TAG, "canceled");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
  }

  /* Capture current volume so on-fire restores it for next session. */
  if (settings_get_volume(&s_pre_timer_db) != ESP_OK) {
    s_pre_timer_db = -10.0f;
  }

  int64_t now = esp_timer_get_time();
  int64_t duration_us = (int64_t)minutes * 60LL * 1000 * 1000;
  s_armed_at_us = now;
  s_fire_at_us = now + duration_us;
  s_active = true;
  s_fading = false;

  esp_timer_start_once(s_fire_timer, (uint64_t)duration_us);
  esp_timer_start_periodic(s_fade_timer, FADE_TICK_MS * 1000);

  ESP_LOGI(TAG, "armed for %d min", minutes);
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

void sleep_timer_get(sleep_timer_state_t *out) {
  if (!out) return;
  out->active = false;
  out->minutes_remaining = 0;
  out->fading = false;
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  if (s_active) {
    int64_t remaining_us = s_fire_at_us - esp_timer_get_time();
    if (remaining_us < 0) remaining_us = 0;
    out->active = true;
    /* Round up so the UI shows e.g. "1 min" until the very last second. */
    out->minutes_remaining = (int)((remaining_us + (60LL * 1000 * 1000) - 1) /
                                    (60LL * 1000 * 1000));
    out->fading = s_fading;
  }
  xSemaphoreGive(s_mutex);
}
