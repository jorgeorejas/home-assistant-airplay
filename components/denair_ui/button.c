/**
 * @file button.c
 * @brief Center-button (GPIO0, active-low) with debounce and hold detection.
 *
 * GPIO0 is a strapping pin; the HA Voice PE externally pulls it up, so
 * the firmware only needs a pull-up config and to watch for low transitions.
 * Short press (<1.5 s) triggers the short-press callback. Hold ≥1.5 s
 * triggers the long-press callback (used by Phase 2 to cycle LED modes).
 */

#include "ui_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_GPIO         0
#define DEBOUNCE_MS         20
#define LONG_PRESS_MS       1500

static const char TAG[] = "denair_button";

static void (*s_on_short)(void) = NULL;
static void (*s_on_long)(void) = NULL;
static TaskHandle_t s_task = NULL;

static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  BaseType_t woken = pdFALSE;
  if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
  portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg) {
  (void)arg;
  for (;;) {
    /* Wait for an edge (press or release) */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /* Debounce */
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    if (gpio_get_level(BUTTON_GPIO) != 0) {
      /* bounce or spurious — ignore */
      continue;
    }

    int64_t pressed_at = esp_timer_get_time();
    bool long_fired = false;

    /* Poll every 50 ms while pressed; fire long-press at the threshold */
    while (gpio_get_level(BUTTON_GPIO) == 0) {
      int64_t elapsed = esp_timer_get_time() - pressed_at;
      if (!long_fired && elapsed >= (int64_t)LONG_PRESS_MS * 1000) {
        long_fired = true;
        if (s_on_long) s_on_long();
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!long_fired) {
      if (s_on_short) s_on_short();
    }
  }
}

esp_err_t denair_button_start(void (*on_short)(void),
                              void (*on_long)(void)) {
  s_on_short = on_short;
  s_on_long = on_long;

  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << BUTTON_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) return err;

  BaseType_t ok = xTaskCreate(button_task, "denair_btn", 3072, NULL, 8, &s_task);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

  ESP_LOGI(TAG, "center button ready (GPIO %d, active low)", BUTTON_GPIO);
  return ESP_OK;
}
