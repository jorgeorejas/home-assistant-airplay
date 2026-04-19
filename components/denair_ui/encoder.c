/**
 * @file encoder.c
 * @brief GPIO-interrupt quadrature decoder for the HA Voice PE dial.
 *
 * Runs as an ISR that notifies a dedicated task. The task walks a
 * quadrature state machine and calls the registered callback once per
 * detent (two quadrature transitions — matches resolution:2 in the
 * reference yaml). Simple, bounce-tolerant enough for a mechanical
 * encoder at human turn-speeds.
 */

#include "ui_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

#define ENCODER_A_GPIO 16
#define ENCODER_B_GPIO 18

static const char TAG[] = "denair_encoder";

static void (*s_on_turn)(int) = NULL;
static TaskHandle_t s_task = NULL;

/* Quadrature lookup: index = (old_state << 2) | new_state.
 * Returns +1 for one valid CW transition, -1 for CCW, 0 for no-op or bounce.
 * Classic table; emits per single quadrature edge. */
static const int8_t s_qtab[16] = {
    0, -1, +1,  0,
   +1,  0,  0, -1,
   -1,  0,  0, +1,
    0, +1, -1,  0,
};

static void IRAM_ATTR encoder_isr(void *arg) {
  (void)arg;
  BaseType_t woken = pdFALSE;
  if (s_task) {
    vTaskNotifyGiveFromISR(s_task, &woken);
  }
  portYIELD_FROM_ISR(woken);
}

static void encoder_task(void *arg) {
  (void)arg;
  uint8_t state = (gpio_get_level(ENCODER_A_GPIO) << 1) |
                  gpio_get_level(ENCODER_B_GPIO);
  int accum = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* Drain all pending quadrature edges. */
    for (;;) {
      uint8_t ns = (gpio_get_level(ENCODER_A_GPIO) << 1) |
                   gpio_get_level(ENCODER_B_GPIO);
      if (ns == state) break;
      int8_t d = s_qtab[(state << 2) | ns];
      accum += d;
      state = ns;
      /* One detent on the HA Voice PE dial = 4 quadrature edges (common for
       * this class of encoder). Fire when we've accumulated that much. */
      if (accum >= 4) {
        if (s_on_turn) s_on_turn(+1);
        accum -= 4;
      } else if (accum <= -4) {
        if (s_on_turn) s_on_turn(-1);
        accum += 4;
      }
    }
  }
}

esp_err_t denair_encoder_start(void (*on_turn)(int)) {
  s_on_turn = on_turn;

  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << ENCODER_A_GPIO) | (1ULL << ENCODER_B_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) return err;

  BaseType_t ok = xTaskCreate(encoder_task, "denair_enc", 3072, NULL, 10, &s_task);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  gpio_isr_handler_add(ENCODER_A_GPIO, encoder_isr, NULL);
  gpio_isr_handler_add(ENCODER_B_GPIO, encoder_isr, NULL);

  ESP_LOGI(TAG, "encoder ready (A=%d B=%d, 4 quad-edges / detent)",
           ENCODER_A_GPIO, ENCODER_B_GPIO);
  return ESP_OK;
}
