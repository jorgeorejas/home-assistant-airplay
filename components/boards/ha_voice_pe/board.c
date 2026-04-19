/**
 * @file board.c
 * @brief HA Voice PE (Home Assistant AirPlay) board init
 *
 * Phase 0 scope: bring up I2C to the TLV320AIC3204 DAC, configure the
 * internal-speaker amp GPIO, seed WiFi credentials from wifi_config.h
 * with two-SSID fallback. Jack-detect, encoder, and LED-ring in Phase 1/2.
 */

#include "iot_board.h"

#include "dac.h"
#include "dac_tlv320aic3204.h"
#include "settings.h"
#include "wifi.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <string.h>

#if __has_include("wifi_config.h")
# include "wifi_config.h"
# define HA_AIRPLAY_HAVE_WIFI_CONFIG 1
#else
# define HA_AIRPLAY_HAVE_WIFI_CONFIG 0
#endif

#ifndef WIFI_CONNECT_TIMEOUT_MS
# define WIFI_CONNECT_TIMEOUT_MS 25000
#endif

static const char TAG[] = "HA-Voice-PE";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* ------------------------------------------------------------------ */
/*  WiFi two-SSID fallback supervisor                                  */
/* ------------------------------------------------------------------ */

#if HA_AIRPLAY_HAVE_WIFI_CONFIG
typedef struct {
  const char *ssid;
  const char *password;
} wifi_net_t;

static const wifi_net_t s_wifi_nets[] = {
    { WIFI_SSID_1, WIFI_PASSWORD_1 },
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
};
#define WIFI_NETS_COUNT (int)(sizeof(s_wifi_nets) / sizeof(s_wifi_nets[0]))

static int s_wifi_active_idx = 0;
static esp_timer_handle_t s_wifi_fallback_timer = NULL;

static int next_configured_net(int from_idx) {
  for (int i = 1; i <= WIFI_NETS_COUNT; i++) {
    int candidate = (from_idx + i) % WIFI_NETS_COUNT;
    if (s_wifi_nets[candidate].ssid && s_wifi_nets[candidate].ssid[0] != '\0') {
      return candidate;
    }
  }
  return -1;
}

static void apply_wifi_net(int idx) {
  const wifi_net_t *n = &s_wifi_nets[idx];
  ESP_LOGI(TAG, "WiFi fallback: switching to SSID='%s' (slot %d)", n->ssid, idx + 1);

  /* Persist so re-flash of firmware starts clean, and so reboots remember
   * which net was last working. */
  settings_set_wifi_credentials(n->ssid, n->password);

  /* Update live STA config and kick the reconnect loop. */
  wifi_config_t cfg = {0};
  strncpy((char *)cfg.sta.ssid, n->ssid, sizeof(cfg.sta.ssid) - 1);
  strncpy((char *)cfg.sta.password, n->password, sizeof(cfg.sta.password) - 1);
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_disconnect();
  esp_wifi_connect();

  s_wifi_active_idx = idx;
}

static void wifi_fallback_tick(void *arg) {
  (void)arg;
  if (wifi_is_connected()) {
    ESP_LOGI(TAG, "WiFi fallback: connected on SSID='%s', supervisor stopping",
             s_wifi_nets[s_wifi_active_idx].ssid);
    return;
  }
  int next = next_configured_net(s_wifi_active_idx);
  if (next < 0 || next == s_wifi_active_idx) {
    ESP_LOGW(TAG, "WiFi fallback: no alternate SSID configured, staying on '%s'",
             s_wifi_nets[s_wifi_active_idx].ssid);
    return;
  }
  apply_wifi_net(next);

  /* Re-arm for another round; caps out implicitly when wifi_is_connected()
   * returns true on the next tick. */
  esp_timer_start_once(s_wifi_fallback_timer,
                       (uint64_t)WIFI_CONNECT_TIMEOUT_MS * 1000);
}

static void start_wifi_fallback_supervisor(void) {
  int first = next_configured_net(-1);
  if (first < 0) {
    ESP_LOGW(TAG, "WiFi fallback: no SSIDs configured in wifi_config.h");
    return;
  }
  int alt = next_configured_net(first);
  if (alt == first) {
    ESP_LOGI(TAG, "WiFi fallback: only one SSID configured, no fallback needed");
    return;
  }

  const esp_timer_create_args_t args = {
      .callback = wifi_fallback_tick,
      .name = "ha_airplay_wifi_fb",
  };
  esp_err_t err = esp_timer_create(&args, &s_wifi_fallback_timer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "WiFi fallback: esp_timer_create failed: %s",
             esp_err_to_name(err));
    return;
  }
  esp_timer_start_once(s_wifi_fallback_timer,
                       (uint64_t)WIFI_CONNECT_TIMEOUT_MS * 1000);
  ESP_LOGI(TAG, "WiFi fallback: armed (swap SSID after %d ms if not connected)",
           WIFI_CONNECT_TIMEOUT_MS);
}
#endif /* HA_AIRPLAY_HAVE_WIFI_CONFIG */

/* Seed NVS WiFi credentials from wifi_config.h (Home Assistant AirPlay hardcodes them at
 * compile time per the PRD — captive portal is a fallback only). Runs on
 * every boot so re-flashing with updated credentials is the source of truth.
 * Falls through to upstream captive-portal behaviour if wifi_config.h is
 * absent or WIFI_SSID_1 is empty. */
static void seed_wifi_credentials_from_config(void) {
#if HA_AIRPLAY_HAVE_WIFI_CONFIG
  int first = next_configured_net(-1);
  if (first < 0) {
    ESP_LOGW(TAG, "wifi_config.h present but no non-empty SSID found; "
                  "falling back to captive-portal setup");
    return;
  }
  const wifi_net_t *n = &s_wifi_nets[first];
  esp_err_t err = settings_set_wifi_credentials(n->ssid, n->password);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "seeded WiFi credentials for SSID='%s' from wifi_config.h",
             n->ssid);
    s_wifi_active_idx = first;
  } else {
    ESP_LOGW(TAG, "seeding WiFi credentials failed: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGW(TAG, "wifi_config.h not found; using captive-portal setup. "
                "Copy wifi_config.h.example to wifi_config.h and rebuild to "
                "pin credentials at compile time.");
#endif
}

/* ------------------------------------------------------------------ */
/*  Board interface                                                    */
/* ------------------------------------------------------------------ */

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
  /* Start with the internal amp ON. Home Assistant AirPlay's jack-detect logic flips this to
   * LOW when the 3.5 mm jack is inserted (Phase 1). */
  ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1), TAG,
                      "amp enable default high");
  return ESP_OK;
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) return ESP_OK;

  /* Seed WiFi credentials from compile-time config before wifi_init_apsta
   * runs in app_main. Supervisor timer fires later (after WiFi is up) and
   * flips to the fallback SSID if the primary doesn't associate. */
  seed_wifi_credentials_from_config();
#if HA_AIRPLAY_HAVE_WIFI_CONFIG
  start_wifi_fallback_supervisor();
#endif

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

  /* Home Assistant AirPlay LED ring + encoder/button init are called from main/main.c
   * right after iot_board_init returns. Keeping those references out of
   * libboards.a avoids a link-order issue with ESP-IDF's one-pass
   * linker (ha_airplay_* libs get ordered before libboards.a in the
   * component graph, so symbols referenced from inside libboards.a
   * wouldn't resolve). */

  s_board_initialized = true;
  ESP_LOGI(TAG, "HA Voice PE initialized (Home Assistant AirPlay)");
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
