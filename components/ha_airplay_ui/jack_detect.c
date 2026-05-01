/**
 * @file jack_detect.c
 * @brief 3.5 mm jack-detect on GPIO17. When the jack is inserted, drive
 *        GPIO47 LOW (disable the internal speaker amp); when it's
 *        removed, drive GPIO47 HIGH again. A 5 s LED overlay confirms
 *        the new output destination.
 *
 * Polarity: GPIO17 reads LOW when the jack is inserted (the contact
 * shorts to ground). Pull-up keeps it HIGH when nothing's plugged in.
 * 200 ms debounce per the original PRD §5.3.
 */

#include "ui_internal.h"

#include "ha_airplay_leds.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JACK_GPIO         17
#define AMP_ENABLE_GPIO   47
#define DEBOUNCE_MS       200

static const char TAG[] = "ha_airplay_jack";

static TaskHandle_t s_task = NULL;

static void IRAM_ATTR jack_isr(void *arg) {
  (void)arg;
  BaseType_t woken = pdFALSE;
  if (s_task) {
    vTaskNotifyGiveFromISR(s_task, &woken);
  }
  portYIELD_FROM_ISR(woken);
}

static void apply_state(int level) {
  /* level == 0 means jack inserted → mute internal amp.
   * level == 1 means jack removed   → enable internal amp. */
  bool jack_in = (level == 0);
  gpio_set_level(AMP_ENABLE_GPIO, jack_in ? 0 : 1);
  ha_airplay_leds_show_output_change(jack_in);
}

static void jack_task(void *arg) {
  (void)arg;
  /* Seed initial state so the amp matches whatever is plugged in at boot.
     We deliberately do NOT fire the LED overlay here — the boot animation
     plays during this window and the user expects it to be undisturbed. */
  int last = gpio_get_level(JACK_GPIO);
  bool jack_in = (last == 0);
  gpio_set_level(AMP_ENABLE_GPIO, jack_in ? 0 : 1);
  ESP_LOGI(TAG, "jack-detect seeded: level=%d → %s", last,
           jack_in ? "jack output, internal amp OFF"
                   : "internal speaker, amp ON");

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    int lvl = gpio_get_level(JACK_GPIO);
    if (lvl != last) {
      last = lvl;
      apply_state(lvl);
    }
  }
}

esp_err_t ha_airplay_jack_detect_start(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << JACK_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) {
    return err;
  }

  BaseType_t ok = xTaskCreate(jack_task, "ha_airplay_jack", 3072, NULL, 6,
                              &s_task);
  if (ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }
  gpio_isr_handler_add(JACK_GPIO, jack_isr, NULL);

  ESP_LOGI(TAG, "jack-detect ready (GPIO %d → amp on GPIO %d, %d ms debounce)",
           JACK_GPIO, AMP_ENABLE_GPIO, DEBOUNCE_MS);
  return ESP_OK;
}
