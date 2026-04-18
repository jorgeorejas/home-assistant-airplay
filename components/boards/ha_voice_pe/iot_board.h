/**
 * @file iot_board.h
 * @brief Home Assistant Voice Preview Edition (NC-VK-9727) board definitions
 *
 * Fixed hardware, so pin numbers are baked in rather than pulled from
 * BOARD_TARGET_PATH kconfig knobs. Values sourced from
 * docs/home-assistant-voice-pe-dev/home-assistant-voice.yaml (authoritative
 * upstream pin map — see memory/voice_pe_pinmap.md).
 */

#pragma once

#include "board_common.h"
#include "sdkconfig.h"

#define BOARD_NAME        "HA Voice PE (DenAir)"
#define BOARD_DESCRIPTION "Home Assistant Voice Preview Edition + TLV320AIC3204 DAC, DenAir firmware"

/* ---- I2S (ESP32-S3 master-out to AIC3204 DAC) ---- */
#define BOARD_I2S_BCK_GPIO 8   /* bit clock */
#define BOARD_I2S_WS_GPIO  7   /* word select / LRCLK */
#define BOARD_I2S_DO_GPIO  10  /* serial data out */
#define BOARD_I2S_SCK_GPIO -1  /* MCLK not used — AIC3204 runs on BCLK */
#define BOARD_I2S_GND_GPIO -1
#define BOARD_I2S_VCC_GPIO -1

/* ---- SPDIF not used ---- */
#define BOARD_SPDIF_BCK_GPIO -1
#define BOARD_SPDIF_WS_GPIO  -1
#define BOARD_SPDIF_DO_GPIO  -1

/* ---- LEDs: addressable 12-LED WS2812B ring driven by DenAir's own engine
 *           (not the upstream single-status indicator). Point upstream at -1
 *           so it does not claim the pin. ---- */
#define BOARD_LED_STATUS_GPIO -1
#define BOARD_LED_ERROR_GPIO  -1
#define BOARD_LED_RGB_GPIO    -1   /* DenAir LED engine uses GPIO21 directly */

/* ---- Jack detect + internal amp enable ---- */
#define BOARD_JACK_GPIO        17  /* 3.5 mm jack detect (200 ms debounce) */
#define BOARD_SPKFAULT_GPIO    -1
#define BOARD_AMP_ENABLE_GPIO  47  /* TPA6211A internal speaker amp enable: HIGH = on */
/* DenAir handles amp enable in our DAC driver's enable_speaker op, not via
 * upstream's generic MUTE_GPIO path. */
#define BOARD_MUTE_GPIO        -1
#define BOARD_MUTE_GPIO_LEVEL  0

/* ---- AIC3204 I2C ---- */
#define BOARD_I2C_PORT     I2C_NUM_0
#define BOARD_I2C_SDA_GPIO 5
#define BOARD_I2C_SCL_GPIO 6

/* ---- Rotary encoder (DenAir UI) ---- */
#define BOARD_ENCODER_A_GPIO  16
#define BOARD_ENCODER_B_GPIO  18

/* ---- Battery monitor not used ---- */
#define BOARD_BAT_CHANNEL -1
