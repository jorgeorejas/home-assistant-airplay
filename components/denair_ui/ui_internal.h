#pragma once

#include "esp_err.h"

/* Internal wiring between encoder/button drivers and the UI coordinator.
 * Not part of the public denair_ui.h contract — just in-component glue. */

esp_err_t denair_encoder_start(void (*on_turn)(int direction));
esp_err_t denair_button_start(void (*on_short_press)(void),
                              void (*on_long_press)(void));
