/**
 * @file board.c
 * @brief HA Voice PE (DenAir) board init
 *
 * Phase 0 scope: bring up I2C to the TLV320AIC3204 DAC, configure the
 * internal-speaker amp GPIO, and hand the I2C bus to the DAC driver. Jack-
 * detect, encoder, and LED-ring work land in Phase 1/2.
 */

#include "iot_board.h"

#include "dac.h"
#include "dac_tlv320aic3204.h"
#include "settings.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include <string.h>

#if __has_include("wifi_config.h")
# include "wifi_config.h"
# define DENAIR_HAVE_WIFI_CONFIG 1
#else
# define DENAIR_HAVE_WIFI_CONFIG 0
#endif

static const char TAG[] = "HA-Voice-PE";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;

const char *iot_board_get_info(void) { return BOARD_NAME; }

bool iot_board_is_init(void) { return s_board_initialized; }

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_I2C_DAC_ID:
    return (board_res_handle_t)s_i2c_bus;
  default:
    return NULL;
  }
}

static esp_err_t configure_amp_enable_gpio(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << BOARD_AMP_ENABLE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "amp enable gpio config");
  /* Start with the internal amp ON. DenAir's jack-detect logic flips this to
   * LOW when the 3.5 mm jack is inserted (Phase 1). */
  ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1), TAG,
                      "amp enable default high");
  return ESP_OK;
}

/* Seed NVS WiFi credentials from wifi_config.h (DenAir hardcodes them at
 * compile time per the PRD — captive portal is a fallback only). Runs on
 * every boot so re-flashing with updated credentials is the source of truth.
 * Falls through to upstream captive-portal behaviour if wifi_config.h is
 * absent or WIFI_SSID_1 is empty. */
static void seed_wifi_credentials_from_config(void) {
#if DENAIR_HAVE_WIFI_CONFIG
  const char *ssid = WIFI_SSID_1;
  const char *pw = WIFI_PASSWORD_1;
  if (ssid == NULL || ssid[0] == '\0') {
    ESP_LOGW(TAG, "wifi_config.h present but WIFI_SSID_1 is empty; "
                  "falling back to captive-portal setup");
    return;
  }
  esp_err_t err = settings_set_wifi_credentials(ssid, pw);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "seeded WiFi credentials for SSID='%s' from wifi_config.h", ssid);
  } else {
    ESP_LOGW(TAG, "seeding WiFi credentials failed: %s", esp_err_to_name(err));
  }
  /* TODO(phase1): WIFI_SSID_2 fallback — requires extending upstream wifi.c
   * which today tries one SSID only. */
#else
  ESP_LOGW(TAG, "wifi_config.h not found; using captive-portal setup. "
                "Copy wifi_config.h.example to wifi_config.h and rebuild to "
                "pin credentials at compile time.");
#endif
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) return ESP_OK;

  /* Seed WiFi credentials from compile-time config before wifi_init_apsta
   * runs in app_main. */
  seed_wifi_credentials_from_config();

  /* Register the AIC3204 DAC driver before calling dac_init so dac.c finds
   * an ops table when main calls it. */
  dac_register(&dac_tlv320aic3204_ops);

  /* Internal speaker amp (TPA6211A) enable line */
  ESP_RETURN_ON_ERROR(configure_amp_enable_gpio(), TAG, "amp enable");

  /* I2C master bus for the AIC3204 */
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG,
                      "i2c bus init");
  ESP_LOGI(TAG, "I2C bus %d up (sda=%d scl=%d)", BOARD_I2C_PORT,
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);

  ESP_RETURN_ON_ERROR(dac_init(s_i2c_bus), TAG, "dac init");

  s_board_initialized = true;
  ESP_LOGI(TAG, "HA Voice PE initialized (DenAir)");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) return ESP_OK;

  dac_deinit();

  if (s_i2c_bus) {
    i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
  }

  gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0);

  s_board_initialized = false;
  return ESP_OK;
}
