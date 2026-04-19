#pragma once

#include "esp_err.h"

/* Internal wiring between encoder/button drivers and the UI coordinator.
 * Not part of the public ha_airplay_ui.h contract — just in-component glue. */

esp_err_t ha_airplay_encoder_start(void (*on_turn)(int direction));

typedef struct {
  void (*short_cb)(void);   /* single click */
  void (*double_cb)(void);  /* two clicks within ~400 ms */
  void (*triple_cb)(void);  /* three clicks within ~400 ms */
  void (*long_cb)(void);    /* held ≥1.5 s — fires once at threshold */
} ha_airplay_button_callbacks_t;

esp_err_t ha_airplay_button_start(const ha_airplay_button_callbacks_t *cbs);
esp_err_t ha_airplay_led_switch_start(void);
