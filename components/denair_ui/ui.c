/**
 * @file ui.c
 * @brief DenAir UI coordinator — wires encoder turns and button presses
 *        into upstream playback_control + denair_leds feedback.
 */

#include "denair_ui.h"
#include "ui_internal.h"

#include "denair_leds.h"
#include "playback_control.h"
#include "settings.h"

#include "esp_log.h"

#include <stdatomic.h>

static const char TAG[] = "denair_ui";

/* Upstream uses -30..0 dB as the AirPlay volume range. */
#define VOL_MIN_DB -30.0f
#define VOL_MAX_DB 0.0f

static _Atomic bool s_muted = false;

static void show_current_volume_on_leds(int hold_ms) {
  float db;
  if (settings_get_volume(&db) != ESP_OK) db = -15.0f;
  float frac = (db - VOL_MIN_DB) / (VOL_MAX_DB - VOL_MIN_DB);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  denair_leds_show_volume(frac, hold_ms);
}

static void on_encoder_turn(int direction) {
  if (direction > 0) {
    playback_control_volume_up();
  } else if (direction < 0) {
    playback_control_volume_down();
  }
  /* If we were muted, turning the encoder unmutes. */
  if (atomic_exchange(&s_muted, false)) {
    denair_leds_set_muted(false);
  }
  show_current_volume_on_leds(2000);
}

static void on_button_short(void) {
  /* On AirPlay 2 this is a local mute toggle per playback_control.c */
  playback_control_play_pause();
  bool now_muted = !atomic_load(&s_muted);
  atomic_store(&s_muted, now_muted);
  denair_leds_set_muted(now_muted);
  ESP_LOGI(TAG, "button short press → %s", now_muted ? "muted" : "unmuted");
}

static void on_button_long(void) {
  /* Phase 2 will cycle LED modes here. Log-only for now so the event is
   * visible on serial during bring-up. */
  ESP_LOGI(TAG, "button long press (Phase 2: LED mode cycle placeholder)");
}

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
  /* Paint the initial volume once so boot produces visible LED feedback
   * that the ring engine is alive. Short overlay — fades into idle. */
  show_current_volume_on_leds(1500);
  ESP_LOGI(TAG, "UI ready");
  return ESP_OK;
}
