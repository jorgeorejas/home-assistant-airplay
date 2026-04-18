/**
 * @file dac_tlv320aic3204.c
 * @brief TI TLV320AIC3204 DAC driver for DenAir / HA Voice PE.
 *
 * Phase 0 scaffold: establishes the I2C transport, probes the chip, and
 * exposes the dac_ops_t contract with minimal working init + volume +
 * power. Output routing (headphone vs line-out vs class-D speaker drive)
 * is stubbed until hardware probing settles which AIC3204 output pins are
 * wired to the jack vs the internal TPA6211A on this board.
 *
 * Datasheet: https://www.ti.com/lit/ds/symlink/tlv320aic3204.pdf
 *
 * Reference register sequence cribbed from ESPHome's aic3204 component,
 * which is known to work on this exact hardware (see
 * vivla-voice-pe.yaml:96-132 in the v0.1-voice tag).
 */

#include "dac_tlv320aic3204.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdbool.h>

static const char TAG[] = "AIC3204";

#define AIC3204_I2C_ADDR CONFIG_TLV320AIC3204_I2C_ADDR
#define I2C_TIMEOUT_MS   50
#define I2C_SPEED_HZ     400000

/* Page-0 registers */
#define REG_PAGE_SEL         0x00
#define REG_SOFT_RESET       0x01
#define REG_DAC_LEFT_VOL     0x41 /* 65 dec */
#define REG_DAC_RIGHT_VOL    0x42 /* 66 dec */
#define REG_DAC_CHANNEL_SETUP 0x3F /* 63 dec */
#define REG_DAC_MUTE         0x40 /* 64 dec */

static i2c_master_dev_handle_t s_dev = NULL;
static float s_volume_db_cache = -96.0f;
static bool s_initialized = false;

/* ---------- Low-level I2C helpers ---------- */

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t select_page(uint8_t page) {
  return write_reg(REG_PAGE_SEL, page);
}

/* ---------- Init sequence ---------- */

static esp_err_t soft_reset(void) {
  ESP_RETURN_ON_ERROR(select_page(0), TAG, "page 0");
  ESP_RETURN_ON_ERROR(write_reg(REG_SOFT_RESET, 0x01), TAG, "soft reset");
  vTaskDelay(pdMS_TO_TICKS(10));
  return ESP_OK;
}

static esp_err_t init_codec(void) {
  /* Phase 0 minimal bring-up. Real clocking, PLL, output routing, and
   * channel path setup lands after the first audio-out bring-up on bench.
   * The list below covers the registers that must be touched before the
   * chip will pass audio even in its simplest mode. Leaving them as TODOs
   * keeps this file honest about what Phase 0 actually does. */

  /* TODO(phase0): PLL setup — page 0 regs 4-6 (clock source, P/R/J/D) */
  /* TODO(phase0): NDAC/MDAC/DOSR dividers — page 0 regs 11-14 */
  /* TODO(phase0): CODEC_INTERFACE — page 0 reg 27 (I2S, 16-bit, BCLK slave) */
  /* TODO(phase0): DAC data-path — page 0 reg 63 (DAC channel setup) */
  /* TODO(phase0): Output routing — page 1 regs 12-16 (HP/SPK routing, gains) */

  /* Default the digital volume to the max-volume ceiling (e.g. -6 dB at
   * CONFIG_TLV320AIC3204_MAX_VOLUME_DB). AirPlay client will call
   * dac_set_volume() to attenuate below this. */
  ESP_RETURN_ON_ERROR(select_page(0), TAG, "page 0 for vol");
  const int8_t max_vol_db = CONFIG_TLV320AIC3204_MAX_VOLUME_DB;
  /* Page 0 reg 65/66 are signed 8-bit, 0.5 dB step, 0x00 = 0 dB. */
  uint8_t vol_code = (uint8_t)(max_vol_db * 2); /* two's-complement wrap */
  ESP_RETURN_ON_ERROR(write_reg(REG_DAC_LEFT_VOL, vol_code), TAG, "left vol");
  ESP_RETURN_ON_ERROR(write_reg(REG_DAC_RIGHT_VOL, vol_code), TAG, "right vol");

  s_volume_db_cache = (float)max_vol_db;
  return ESP_OK;
}

/* ---------- dac_ops_t implementations ---------- */

static esp_err_t aic3204_init(void *i2c_bus) {
  if (s_initialized) return ESP_OK;
  if (i2c_bus == NULL) {
    ESP_LOGE(TAG, "i2c_bus handle is NULL");
    return ESP_ERR_INVALID_ARG;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = AIC3204_I2C_ADDR,
      .scl_speed_hz = I2C_SPEED_HZ,
  };
  esp_err_t err = i2c_master_bus_add_device(
      (i2c_master_bus_handle_t)i2c_bus, &dev_cfg, &s_dev);
  ESP_RETURN_ON_ERROR(err, TAG, "add aic3204 on i2c");

  ESP_RETURN_ON_ERROR(soft_reset(), TAG, "soft reset");
  ESP_RETURN_ON_ERROR(init_codec(), TAG, "codec init");

  s_initialized = true;
  ESP_LOGI(TAG, "AIC3204 online at I2C addr 0x%02X", AIC3204_I2C_ADDR);
  return ESP_OK;
}

static esp_err_t aic3204_deinit(void) {
  if (!s_initialized) return ESP_OK;
  if (s_dev) {
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
  }
  s_initialized = false;
  return ESP_OK;
}

static void aic3204_set_volume(float volume_db) {
  if (!s_initialized) return;

  /* AirPlay scale is -30..0 dB. Map linearly onto the ceiling defined by
   * CONFIG_TLV320AIC3204_MAX_VOLUME_DB down to -63.5 dB (chip floor). */
  const float ceil_db = (float)CONFIG_TLV320AIC3204_MAX_VOLUME_DB;
  const float floor_db = -63.5f;
  float db = ceil_db + (volume_db * (ceil_db - floor_db) / 30.0f);
  if (db > ceil_db) db = ceil_db;
  if (db < floor_db) db = floor_db;

  /* 0.5 dB step, signed; 0x00 = 0 dB, 0x30 = +24 dB, 0x81 = -63.5 dB */
  int8_t step = (int8_t)lroundf(db * 2.0f);
  uint8_t vol_code = (uint8_t)step;

  select_page(0);
  write_reg(REG_DAC_LEFT_VOL, vol_code);
  write_reg(REG_DAC_RIGHT_VOL, vol_code);
  s_volume_db_cache = db;
  ESP_LOGD(TAG, "set_volume airplay_db=%.1f → dac_db=%.1f code=0x%02X",
           volume_db, db, vol_code);
}

static void aic3204_set_power_mode(dac_power_mode_t mode) {
  if (!s_initialized) return;
  select_page(0);
  switch (mode) {
  case DAC_POWER_ON:
    /* TODO(phase0): enable DAC channels via reg 63 + clear mute in reg 64 */
    write_reg(REG_DAC_MUTE, 0x00);
    break;
  case DAC_POWER_STANDBY:
    write_reg(REG_DAC_MUTE, 0x0C); /* mute L+R, channels stay powered */
    break;
  case DAC_POWER_OFF:
    /* TODO(phase0): power down DAC channels via reg 63 */
    write_reg(REG_DAC_MUTE, 0x0C);
    break;
  }
}

static void aic3204_enable_speaker(bool enable) {
  /* GPIO47 (amp enable) lives on the board layer, not here — see
   * components/boards/ha_voice_pe/board.c. This callback is a hook for
   * future routing (e.g., SPK output path enable/disable in the codec). */
  (void)enable;
  /* TODO(phase1): toggle AIC3204 SPK output path if / when we enable it */
}

static void aic3204_enable_line_out(bool enable) {
  /* Line-out is the primary path on DenAir (→ 3.5 mm jack → Denon). Always
   * routed and enabled once the chip is up; caller toggles via volume / mute. */
  (void)enable;
}

/* ---------- Ops table ---------- */

const dac_ops_t dac_tlv320aic3204_ops = {
    .init = aic3204_init,
    .deinit = aic3204_deinit,
    .set_volume = aic3204_set_volume,
    .set_power_mode = aic3204_set_power_mode,
    .enable_speaker = aic3204_enable_speaker,
    .enable_line_out = aic3204_enable_line_out,
};
