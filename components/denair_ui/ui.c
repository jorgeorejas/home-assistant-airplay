/**
 * @file ui.c
 * @brief DenAir UI coordinator.
 *
 * Button mapping (modern-iOS reality: DACP headers aren't sent to
 * non-MFi devices, so play/pause and track controls can't reach the
 * iPhone — we still wire them up for forward-compat and for the local
 * pause/mute fallbacks that upstream provides).
 *
 *   Short press    → playback_control_play_pause  (local pause/mute in AP2)
 *   Double click   → playback_control_next        (DACP → iOS if ever enabled)
 *   Triple click   → playback_control_prev        (DACP → iOS if ever enabled)
 *   Long press     → local DAC mute toggle + LED muted indicator
 *
 * Encoder:
 *   Turn           → playback_control_volume_{up,down}
 *                    (local DAC volume + attempts DACP push to iPhone slider)
 */

#include "denair_ui.h"
#include "ui_internal.h"

#include "dac.h"
#include "denair_leds.h"
#include "playback_control.h"
#include "rtsp_events.h"
#include "settings.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdatomic.h>

static const char TAG[] = "denair_ui";

#define VOL_MIN_DB (-30.0f)
#define VOL_MAX_DB (0.0f)

/* Long-press mute is separate from the upstream "play_pause = local mute"
 * path: it goes through the DAC's STANDBY power mode so it survives play/
 * pause toggling. */
static _Atomic bool s_hard_muted = false;

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

/* ---------- Encoder ---------- */

static void on_encoder_turn(int direction) {
  if (direction > 0) {
    playback_control_volume_up();
  } else if (direction < 0) {
    playback_control_volume_down();
  }
  /* Turning the encoder always clears hard-mute. */
  if (atomic_exchange(&s_hard_muted, false)) {
    dac_set_power_mode(DAC_POWER_ON);
    denair_leds_set_muted(false);
  }
  show_current_volume_on_leds(2000);
}

/* ---------- Button ---------- */

static void on_button_short(void) {
  ESP_LOGI(TAG, "button: short → play/pause (local)");
  playback_control_play_pause();
}

static void on_button_double(void) {
  ESP_LOGI(TAG, "button: double → next track (DACP best-effort)");
  playback_control_next();
}

static void on_button_triple(void) {
  ESP_LOGI(TAG, "button: triple → previous track (DACP best-effort)");
  playback_control_prev();
}

static void on_button_long(void) {
  bool now_muted = !atomic_load(&s_hard_muted);
  atomic_store(&s_hard_muted, now_muted);
  ESP_LOGI(TAG, "button: long → %s (local)", now_muted ? "muted" : "unmuted");
  dac_set_power_mode(now_muted ? DAC_POWER_STANDBY : DAC_POWER_ON);
  denair_leds_set_muted(now_muted);
}

/* ---------- RTSP → LED state ---------- */

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
    break;
  }
}

/* ---------- iOS volume poll ---------- */

static void volume_poll_task(void *arg) {
  (void)arg;
  float last = current_volume_fraction();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    float now = current_volume_fraction();
    if (fabsf(now - last) > 0.005f) {
      denair_leds_show_volume(now, 2000);
      last = now;
    }
  }
}

/* ---------- Init ---------- */

esp_err_t denair_ui_init(void) {
  esp_err_t err = denair_encoder_start(on_encoder_turn);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "encoder start: %s", esp_err_to_name(err));
    return err;
  }

  denair_button_callbacks_t btn_cbs = {
      .short_cb = on_button_short,
      .double_cb = on_button_double,
      .triple_cb = on_button_triple,
      .long_cb = on_button_long,
  };
  err = denair_button_start(&btn_cbs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "button start: %s", esp_err_to_name(err));
    return err;
  }

  esp_err_t sw_err = denair_led_switch_start();
  if (sw_err != ESP_OK) {
    ESP_LOGW(TAG, "LED on/off switch init failed: %s", esp_err_to_name(sw_err));
  }

  rtsp_events_register(on_rtsp_event, NULL);

  BaseType_t ok = xTaskCreate(volume_poll_task, "denair_volpoll", 3072, NULL,
                              4, NULL);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "volume poll task: out of memory");
  }

  show_current_volume_on_leds(1500);
  ESP_LOGI(TAG, "UI ready");
  return ESP_OK;
}
