/**
 * @file dac_tlv320aic3204.c
 * @brief TI TLV320AIC3204 DAC driver for Home Assistant AirPlay / HA Voice PE.
 *
 * Init sequence is a straight port of ESPHome's aic3204 component
 * (esphome/components/aic3204/aic3204.cpp on github.com/esphome/esphome).
 * That component is known to work on this exact hardware.
 *
 * Wiring on HA Voice PE:
 *   - I2C addr 0x18 (SDA=GPIO5, SCL=GPIO6, 400 kHz)
 *   - I2S in from ESP32-S3: BCK=GPIO8, LRCLK=GPIO7, DIN=GPIO10, master
 *   - HPL/HPR analog out → 3.5 mm jack (→ Denon AUX)
 *   - LOL/LOR line out → TPA6211A internal speaker amp (enabled by
 *     GPIO47 which is driven from components/boards/ha_voice_pe/board.c,
 *     not here)
 *
 * Datasheet: https://www.ti.com/lit/ds/symlink/tlv320aic3204.pdf
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

/* ---------- Register addresses (from ESPHome's aic3204.h) ---------- */

/* Page 0 */
#define AIC3204_PAGE_CTRL      0x00
#define AIC3204_SW_RST         0x01
#define AIC3204_CLK_PLL1       0x04
#define AIC3204_CLK_PLL2       0x05
#define AIC3204_CLK_PLL3       0x06
#define AIC3204_NDAC           0x0B
#define AIC3204_MDAC           0x0C
#define AIC3204_DOSR           0x0E
#define AIC3204_CODEC_IF       0x1B
#define AIC3204_AUDIO_IF_4     0x1F
#define AIC3204_AUDIO_IF_5     0x20
#define AIC3204_SCLK_MFP3      0x38
#define AIC3204_DAC_SIG_PROC   0x3C
#define AIC3204_DAC_CH_SET1    0x3F
#define AIC3204_DAC_CH_SET2    0x40
#define AIC3204_DACL_VOL_D     0x41
#define AIC3204_DACR_VOL_D     0x42

/* Page 1 */
#define AIC3204_PWR_CFG        0x01
#define AIC3204_LDO_CTRL       0x02
#define AIC3204_PLAY_CFG1      0x03
#define AIC3204_PLAY_CFG2      0x04
#define AIC3204_OP_PWR_CTRL    0x09
#define AIC3204_CM_CTRL        0x0A
#define AIC3204_HPL_ROUTE      0x0C
#define AIC3204_HPR_ROUTE      0x0D
#define AIC3204_LOL_ROUTE      0x0E
#define AIC3204_LOR_ROUTE      0x0F
#define AIC3204_HPL_GAIN       0x10
#define AIC3204_HPR_GAIN       0x11
#define AIC3204_LOL_DRV_GAIN   0x12
#define AIC3204_LOR_DRV_GAIN   0x13
#define AIC3204_HP_START       0x14
#define AIC3204_REF_STARTUP    0x7B

/* ---------- State ---------- */

static i2c_master_dev_handle_t s_dev = NULL;
static float s_volume_db_cache = 0.0f;
static bool s_initialized = false;
static bool s_muted = false;

/* ---------- I2C helpers ---------- */

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "i2c write reg=0x%02X val=0x%02X failed: %s", reg, value,
             esp_err_to_name(err));
  }
  return err;
}

static esp_err_t select_page(uint8_t page) {
  return write_reg(AIC3204_PAGE_CTRL, page);
}

/* ---------- Init sequence (port of ESPHome aic3204::setup) ---------- */

static esp_err_t setup_codec(void) {
  /* --- Page 0: digital side, clocking, DAC processing --- */
  ESP_RETURN_ON_ERROR(select_page(0),                         TAG, "page 0");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_SW_RST,       0x01),  TAG, "soft reset");
  vTaskDelay(pdMS_TO_TICKS(10));

  /* PLL / clocking: NDAC=2, MDAC=2, DOSR=128, power-on bit set.
   * These match ESPHome's working config on this hardware; BCLK is the
   * implicit clock source — no external MCLK wired. */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_NDAC,         0x82),  TAG, "NDAC");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_MDAC,         0x82),  TAG, "MDAC");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DOSR,         0x80),  TAG, "DOSR");

  /* Codec interface: I2S, 32-bit word length, BCLK/WCLK slave from MCU */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_CODEC_IF,     0x30),  TAG, "CODEC_IF");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_SCLK_MFP3,    0x02),  TAG, "SCLK_MFP3");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_AUDIO_IF_4,   0x01),  TAG, "AUDIO_IF_4");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_AUDIO_IF_5,   0x01),  TAG, "AUDIO_IF_5");

  /* DAC signal processing block PRB_P1 (standard stereo) */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DAC_SIG_PROC, 0x01),  TAG, "DAC_SIG_PROC");

  /* --- Page 1: analog side, power, output routing --- */
  ESP_RETURN_ON_ERROR(select_page(1),                         TAG, "page 1");

  /* Analog LDO up, common mode 0.75V */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LDO_CTRL,     0x09),  TAG, "LDO enable AVDD");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_PWR_CFG,      0x08),  TAG, "disable crude AVdd");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LDO_CTRL,     0x01),  TAG, "LDO master power");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_CM_CTRL,      0x40),  TAG, "CM = 0.75V");

  /* DAC PowerTune config (default) */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_PLAY_CFG1,    0x00),  TAG, "PLAY_CFG1");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_PLAY_CFG2,    0x00),  TAG, "PLAY_CFG2");

  /* Reference charging + HP soft-step */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_REF_STARTUP,  0x01),  TAG, "REF 40ms");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_HP_START,     0x25),  TAG, "HP soft step");

  /* Output routing: left DAC → HPL + LOL, right DAC → HPR + LOR.
   * Both paths drive simultaneously — the 3.5 mm jack (HP) and the
   * internal amp (LO). Amp enable is GPIO-gated in board.c. */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_HPL_ROUTE,    0x08),  TAG, "HPL route");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_HPR_ROUTE,    0x08),  TAG, "HPR route");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LOL_ROUTE,    0x08),  TAG, "LOL route");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LOR_ROUTE,    0x08),  TAG, "LOR route");

  /* Driver gains — HP -2 dB (ESPHome default), LO 0 dB, all unmuted */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_HPL_GAIN,     0x3E),  TAG, "HPL gain");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_HPR_GAIN,     0x3E),  TAG, "HPR gain");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LOL_DRV_GAIN, 0x00),  TAG, "LOL gain");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_LOR_DRV_GAIN, 0x00),  TAG, "LOR gain");

  /* Power up HPL, HPR, LOL, LOR output drivers */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_OP_PWR_CTRL,  0x3C),  TAG, "OP power");

  /* Give the HP / LO drivers time to ramp before enabling the DAC channels.
   * ESPHome uses 2.5 s; we match that but note the boot-time cost. */
  vTaskDelay(pdMS_TO_TICKS(2500));

  /* --- Page 0 again: power up DAC channels, set initial volume + unmute --- */
  ESP_RETURN_ON_ERROR(select_page(0),                         TAG, "page 0 dac up");
  /* 0xD4: L + R DAC powered, soft-step 1x per sample, data path left→left etc. */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DAC_CH_SET1,  0xD4),  TAG, "DAC_CH_SET1");

  /* Leave volume at the ceiling; AirPlay will attenuate below this. */
  const int8_t max_vol_db = CONFIG_TLV320AIC3204_MAX_VOLUME_DB;
  uint8_t vol_code = (uint8_t)(max_vol_db * 2);
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DACL_VOL_D,   vol_code), TAG, "L vol");
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DACR_VOL_D,   vol_code), TAG, "R vol");

  /* DAC_CH_SET2: bits[3:2] mute, bits[6:4] auto-mute mode. 0x00 = unmuted, no auto-mute. */
  ESP_RETURN_ON_ERROR(write_reg(AIC3204_DAC_CH_SET2,  0x00),  TAG, "DAC unmute");

  s_volume_db_cache = (float)max_vol_db;
  s_muted = false;
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
  ESP_RETURN_ON_ERROR(
      i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_bus, &dev_cfg, &s_dev),
      TAG, "i2c add device");

  ESP_RETURN_ON_ERROR(setup_codec(), TAG, "setup codec");

  s_initialized = true;
  ESP_LOGI(TAG, "AIC3204 online @ 0x%02X", AIC3204_I2C_ADDR);
  return ESP_OK;
}

static esp_err_t aic3204_deinit(void) {
  if (!s_initialized) return ESP_OK;

  /* Mute + power down DAC channels before tearing down I2C. */
  select_page(0);
  write_reg(AIC3204_DAC_CH_SET2, 0x0C); /* mute L+R */
  write_reg(AIC3204_DAC_CH_SET1, 0x14); /* power down DACs */

  if (s_dev) {
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
  }
  s_initialized = false;
  return ESP_OK;
}

static void aic3204_set_volume(float volume_db) {
  if (!s_initialized) return;

  /* AirPlay volume range is -30..0 dB. Map linearly onto the DAC ceiling
   * (CONFIG_TLV320AIC3204_MAX_VOLUME_DB) down to -63.5 dB (chip floor). */
  const float ceil_db = (float)CONFIG_TLV320AIC3204_MAX_VOLUME_DB;
  const float floor_db = -63.5f;
  float db = ceil_db + (volume_db * (ceil_db - floor_db) / 30.0f);
  if (db > ceil_db) db = ceil_db;
  if (db < floor_db) db = floor_db;

  /* 0.5 dB step, signed; 0x00 = 0 dB, 0x30 = +24 dB, 0x81 = -63.5 dB */
  int8_t step = (int8_t)lroundf(db * 2.0f);
  uint8_t vol_code = (uint8_t)step;

  select_page(0);
  write_reg(AIC3204_DACL_VOL_D, vol_code);
  write_reg(AIC3204_DACR_VOL_D, vol_code);
  s_volume_db_cache = db;
  ESP_LOGD(TAG, "set_volume airplay_db=%.1f → dac_db=%.1f code=0x%02X",
           volume_db, db, vol_code);
}

static void aic3204_set_power_mode(dac_power_mode_t mode) {
  if (!s_initialized) return;
  select_page(0);
  switch (mode) {
  case DAC_POWER_ON:
    write_reg(AIC3204_DAC_CH_SET1, 0xD4); /* both channels on, soft-step */
    write_reg(AIC3204_DAC_CH_SET2, 0x00); /* unmute */
    s_muted = false;
    break;
  case DAC_POWER_STANDBY:
    write_reg(AIC3204_DAC_CH_SET2, 0x0C); /* mute L+R, keep channels powered */
    s_muted = true;
    break;
  case DAC_POWER_OFF:
    write_reg(AIC3204_DAC_CH_SET2, 0x0C); /* mute */
    write_reg(AIC3204_DAC_CH_SET1, 0x14); /* DACs powered down */
    s_muted = true;
    break;
  }
}

static void aic3204_enable_speaker(bool enable) {
  /* GPIO47 (internal TPA6211A amp enable) is managed by the board layer
   * — see components/boards/ha_voice_pe/board.c. The AIC3204 LO path that
   * feeds the amp is kept always-routed; gating happens at the amp's EN pin. */
  (void)enable;
}

static void aic3204_enable_line_out(bool enable) {
  /* The 3.5 mm jack is the primary Home Assistant AirPlay output and the HP path stays
   * always-routed. Per-session enable/disable happens via mute, not routing. */
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
