/**
 * @file ui.c
 * @brief DenAir UI coordinator — wires encoder turns and button presses
 *        into upstream playback_control + denair_leds feedback, and
 *        maps AirPlay RTSP events onto LED state transitions.
 */

#include "denair_ui.h"
#include "ui_internal.h"

#include "denair_leds.h"
#include "playback_control.h"
#include "rtsp_events.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdatomic.h>

static const char TAG[] = "denair_ui";

/* Upstream uses -30..0 dB as the AirPlay volume range. */
#define VOL_MIN_DB (-30.0f)
#define VOL_MAX_DB (0.0f)

static _Atomic bool s_muted = false;

static float current_volume_fraction(void) {
  float db;
  if (settings_get_volume(&db) != ESP_OK) db = -15.0f;
  float frac = (db - VOL_MIN_DB) / (VOL_MAX_DB - VOL_MIN_DB);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return frac;
}

static void show_current_volume_on_leds(int hold_ms) {
  denair_leds_show_volume(current_volume_fraction(), hold_ms);
}

/* ------------------------------------------------------------------ */
/*  Encoder + button callbacks                                         */
/* ------------------------------------------------------------------ */

static void on_encoder_turn(int direction) {
  if (direction > 0) {
    playback_control_volume_up();
  } else if (direction < 0) {
    playback_control_volume_down();
  }
  if (atomic_exchange(&s_muted, false)) {
    denair_leds_set_muted(false);
  }
  show_current_volume_on_leds(2000);
}

static void on_button_short(void) {
  /* AirPlay 2: play_pause toggles local mute. */
  playback_control_play_pause();
  bool now_muted = !atomic_load(&s_muted);
  atomic_store(&s_muted, now_muted);
  denair_leds_set_muted(now_muted);
  ESP_LOGI(TAG, "button short press → %s", now_muted ? "muted" : "unmuted");
}

static void on_button_long(void) {
  /* Phase 2: LED mode cycle (beat-pulse / spectrum / idle-only). */
  ESP_LOGI(TAG, "button long press (Phase 2 placeholder)");
}

/* ------------------------------------------------------------------ */
/*  iOS volume poll — detects slider drags on the client side          */
/* ------------------------------------------------------------------ */

/* Upstream's airplay_set_volume writes the new volume to NVS and calls
 * dac_set_volume directly. It does not emit an event. The cheapest way
 * to mirror that change on the LEDs is a 5 Hz polling task that notices
 * when the persisted volume drifts. */
static void volume_poll_task(void *arg) {
  (void)arg;
  float last = current_volume_fraction();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    float now = current_volume_fraction();
    if (fabsf(now - last) > 0.005f) {  /* ignore noise */
      denair_leds_show_volume(now, 2000);
      last = now;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  AirPlay RTSP event hookup                                          */
/* ------------------------------------------------------------------ */

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "AirPlay client connected → LED connection flash");
    denair_leds_flash_connection();
    denair_leds_set_playback_state(DENAIR_PLAYBACK_CONNECTED);
    break;
  case RTSP_EVENT_PLAYING:
    ESP_LOGI(TAG, "AirPlay playing → LED beat-pulse mode");
    denair_leds_set_playback_state(DENAIR_PLAYBACK_PLAYING);
    break;
  case RTSP_EVENT_PAUSED:
    ESP_LOGI(TAG, "AirPlay paused → LED idle");
    denair_leds_set_playback_state(DENAIR_PLAYBACK_PAUSED);
    break;
  case RTSP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "AirPlay disconnected → LED disconnect flash + idle");
    denair_leds_set_playback_state(DENAIR_PLAYBACK_DISCONNECTED);
    break;
  case RTSP_EVENT_METADATA:
    /* Track change. Phase 2 may use this for a short highlight. */
    break;
  }
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

esp_err_t denair_ui_init(void) {
  esp_err_t err = denair_encoder_start(on_encoder_turn);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "encoder start: %s", esp_err_to_name(err));
    return err;
  }
  err = denair_button_start(on_button_short, on_button_long);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "button start: %s", esp_err_to_name(err));
    return err;
  }

  /* Register for AirPlay events so LED state follows playback. */
  rtsp_events_register(on_rtsp_event, NULL);

  /* Poll for iOS-side volume changes. */
  BaseType_t ok = xTaskCreate(volume_poll_task, "denair_volpoll", 3072, NULL,
                              4, NULL);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "volume poll task: out of memory (iOS slider won't flash LEDs)");
  }

  /* One-shot "LEDs are alive" volume flash at boot. */
  show_current_volume_on_leds(1500);
  ESP_LOGI(TAG, "UI ready");
  return ESP_OK;
}
