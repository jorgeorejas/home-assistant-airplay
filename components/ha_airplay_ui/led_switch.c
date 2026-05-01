/**
 * @file led_switch.c
 * @brief Slide-switch on GPIO3 (formerly the Voice PE's mic-mute slider),
 *        repurposed as the on/off for the 12-LED ring's *decorative*
 *        animations.
 *
 * Slide LOW → decorative effects (PLAYING beat-pulse, IDLE breath) ON.
 * Slide HIGH → decorative effects OFF, but utility overlays (mute,
 * volume bar, connection sweep) still render so the user keeps the
 * essential feedback. The WS2812B VCC rail is held HIGH for the
 * lifetime of the device; this slide no longer power-gates the ring.
 */

#include "ui_internal.h"

#include "ha_airplay_leds.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SWITCH_GPIO    3
#define DEBOUNCE_MS    40

static const char TAG[] = "ha_airplay_switch";

static TaskHandle_t s_task = NULL;

static void IRAM_ATTR switch_isr(void *arg) {
  (void)arg;
  BaseType_t woken = pdFALSE;
  if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
  portYIELD_FROM_ISR(woken);
}

static void apply_state(int level) {
  /* LOW → decorative on, HIGH → decorative off. (Slide physically toward
   * the former mute position → ambient effects light up.) */
  ha_airplay_leds_set_decorative_enabled(level == 0);
}

static void switch_task(void *arg) {
  (void)arg;
  int last = gpio_get_level(SWITCH_GPIO);
  apply_state(last);
  ESP_LOGI(TAG, "slide-switch seeded: level=%d → decorative %s", last,
           (last == 0) ? "ON" : "OFF");

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    int lvl = gpio_get_level(SWITCH_GPIO);
    if (lvl != last) {
      last = lvl;
      ESP_LOGI(TAG, "slide-switch → level=%d → decorative %s", lvl,
               (lvl == 0) ? "ON" : "OFF");
      apply_state(lvl);
    }
  }
}

esp_err_t ha_airplay_led_switch_start(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << SWITCH_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) return err;

  BaseType_t ok = xTaskCreate(switch_task, "ha_airplay_sw", 3072, NULL, 6, &s_task);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  gpio_isr_handler_add(SWITCH_GPIO, switch_isr, NULL);

  ESP_LOGI(TAG, "LED on/off switch ready (GPIO %d)", SWITCH_GPIO);
  return ESP_OK;
}
