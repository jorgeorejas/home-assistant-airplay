#pragma once

#include "esp_err.h"

/**
 * Home Assistant AirPlay user interface — rotary encoder + center button.
 *
 * Pins (HA Voice PE, baked in):
 *   - Encoder A  = GPIO16
 *   - Encoder B  = GPIO18
 *   - Button     = GPIO0, active-low (strapping pin, externally pulled up)
 *
 * Turning the encoder steps volume via upstream playback_control's
 * volume_up / volume_down functions, then flashes a volume bar on the
 * LED ring. Short-pressing the button calls playback_control_play_pause
 * which, on AirPlay 2, acts as a local mute toggle.
 */
esp_err_t ha_airplay_ui_init(void);
