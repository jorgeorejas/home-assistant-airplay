/**
 * @file button.c
 * @brief Center-button (GPIO0) press detector with multi-click + long-press.
 *
 * DenAir maps the center button to four distinct gestures. Because modern
 * iOS no longer sends DACP headers to non-MFi AirPlay devices, play/pause
 * and next/prev commands cannot reach the iPhone — but short-press still
 * gives a useful *local* pause, and the DACP attempts are harmless no-ops
 * if iOS ever re-enables them.
 *
 *   Short press (<LONG_PRESS_MS)   1 release    → short callback
 *   Short press × 2 inside BURST   2 releases   → double callback
 *   Short press × 3 inside BURST   3 releases   → triple callback
 *   Held ≥LONG_PRESS_MS            any time     → long callback (one-shot)
 *
 * Implementation: ISR wakes a task on each GPIO edge. The task runs a
 * small state machine with a BURST_MS inter-click window. No timers.
 */

#include "ui_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_GPIO    0
#define DEBOUNCE_MS    20
#define LONG_PRESS_MS  1500
#define BURST_MS       400   /* inter-click window for multi-click detection */

static const char TAG[] = "denair_button";

static denair_button_callbacks_t s_cbs;
static TaskHandle_t s_task = NULL;

static void IRAM_ATTR button_isr(void *arg) {
  (void)arg;
  BaseType_t woken = pdFALSE;
  if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
  portYIELD_FROM_ISR(woken);
}

/* Wait for the next notify or a timeout. Returns true if notified, false
 * if timed out. */
static inline bool wait_notify(uint32_t timeout_ms) {
  return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0;
}

static inline bool is_pressed(void) { return gpio_get_level(BUTTON_GPIO) == 0; }

static void fire_count(int n) {
  if (!s_cbs.short_cb && !s_cbs.double_cb && !s_cbs.triple_cb) return;
  if (n == 1 && s_cbs.short_cb)  s_cbs.short_cb();
  if (n == 2 && s_cbs.double_cb) s_cbs.double_cb();
  if (n == 3 && s_cbs.triple_cb) s_cbs.triple_cb();
  if (n > 3)                     ESP_LOGD(TAG, "ignored %d-click burst", n);
}

static void button_task(void *arg) {
  (void)arg;
  for (;;) {
    /* Wait forever for the first edge. */
    wait_notify(portMAX_DELAY);

    /* Debounce. */
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    if (!is_pressed()) continue;  /* spurious / bounce */

    /* Count presses within a burst window. */
    int count = 0;
    bool long_fired = false;

    while (true) {
      /* --- Button is currently pressed. Wait for release or long-press. --- */
      int64_t pressed_at = esp_timer_get_time();
      while (is_pressed()) {
        int64_t held_us = esp_timer_get_time() - pressed_at;
        if (!long_fired && held_us >= (int64_t)LONG_PRESS_MS * 1000) {
          long_fired = true;
          if (s_cbs.long_cb) s_cbs.long_cb();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }

      /* Debounce release edge. */
      vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

      /* If the long-press already fired we don't count this release as a
       * click; consume any accumulated count and restart. */
      if (long_fired) {
        count = 0;
        break;
      }

      count++;

      /* Wait BURST_MS for another press; if one arrives, loop. */
      if (wait_notify(BURST_MS)) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (!is_pressed()) continue;  /* bounce on quiet edge */
        /* Fresh press: loop to measure hold + release. */
        continue;
      }
      /* Timed out — burst is done. */
      break;
    }

    if (count > 0) {
      fire_count(count);
    }
  }
}

esp_err_t denair_button_start(const denair_button_callbacks_t *cbs) {
  if (cbs) s_cbs = *cbs;

  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << BUTTON_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) return err;

  BaseType_t ok = xTaskCreate(button_task, "denair_btn", 3584, NULL, 8, &s_task);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

  ESP_LOGI(TAG, "center button ready (GPIO %d, short/double/triple/long)",
           BUTTON_GPIO);
  return ESP_OK;
}
