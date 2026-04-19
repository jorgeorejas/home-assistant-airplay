/**
 * @file led_switch.c
 * @brief Hardware mute slide-switch on GPIO3, repurposed in Home Assistant AirPlay as
 *        the master on/off for the 12-LED ring.
 *
 * Home Assistant AirPlay doesn't use the Voice PE's microphone, so the physical mute
 * slider is free. HIGH level → LEDs powered on. LOW level → LEDs
 * powered off (GPIO45 VCC rail goes LOW). An ISR + small task handles
 * debounce; the initial state is read at boot so the ring reflects the
 * switch's physical position immediately.
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
  /* LOW → LEDs on, HIGH → LEDs off. (Slide physically toward the mute
   * position → ring lights up. Polarity confirmed on bench 2026-04-19.) */
  ha_airplay_leds_set_power(level == 0);
}

static void switch_task(void *arg) {
  (void)arg;
  /* Seed current state so the LEDs match the physical slider on boot. */
  int last = gpio_get_level(SWITCH_GPIO);
  apply_state(last);
  ESP_LOGI(TAG, "mute-slide switch seeded: level=%d → LEDs %s", last,
           (last == 0) ? "ON" : "OFF");

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    int lvl = gpio_get_level(SWITCH_GPIO);
    if (lvl != last) {
      last = lvl;
      ESP_LOGI(TAG, "mute-slide switch → level=%d → LEDs %s", lvl,
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

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  gpio_isr_handler_add(SWITCH_GPIO, switch_isr, NULL);

  ESP_LOGI(TAG, "LED on/off switch ready (GPIO %d)", SWITCH_GPIO);
  return ESP_OK;
}
