/*
 * Button driver — polls GPIO4 at 100 Hz, debounces, tracks press count.
 * Active-high wiring (3-pin breakout: OUT -> GPIO, 3V3, GND).
 */

#include "button.h"
#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "diag_log.h"

static const char *TAG = "btn";

static volatile bool     s_pressed = false;
static volatile uint32_t s_count = 0;
static volatile int64_t  s_last_press_us = 0;

static void button_task(void *arg)
{
    bool debounced = false;
    bool candidate = false;
    int64_t candidate_since = 0;

    while (1) {
        int raw = gpio_get_level(BUTTON_GPIO);
        bool now_pressed = (raw == 1);   /* active-high */
        int64_t now = esp_timer_get_time();

        if (now_pressed != candidate) {
            candidate = now_pressed;
            candidate_since = now;
        } else if ((now - candidate_since) > (BUTTON_DEBOUNCE_MS * 1000LL)
                   && candidate != debounced) {
            debounced = candidate;
            s_pressed = debounced;
            if (debounced) {
                s_count++;
                s_last_press_us = now;
                DLOG("[CAMDIAG] BUTTON press #%lu\n", (unsigned long)s_count);
            } else {
                DLOG("[CAMDIAG] BUTTON release (held %lld ms)\n",
                     (long long)((now - s_last_press_us) / 1000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* active-high → pulldown keeps idle low */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    BaseType_t ok = xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL);
    DLOG("[CAMDIAG] button_init GPIO=%d pull-down enabled, task=%s\n",
         BUTTON_GPIO, ok == pdPASS ? "up" : "FAILED");
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool button_is_pressed(void) { return s_pressed; }
uint32_t button_press_count(void) { return s_count; }

uint32_t button_last_press_ms_ago(void)
{
    if (s_last_press_us == 0) return UINT32_MAX;
    int64_t age = (esp_timer_get_time() - s_last_press_us) / 1000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}
