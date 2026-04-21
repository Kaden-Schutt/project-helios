#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BUTTON_GPIO        44     /* D7 on the XIAO Sense, active-high breakout (no SD pullup conflict) */
#define BUTTON_DEBOUNCE_MS 30

/* Start a background task that polls the button pin and maintains state. */
esp_err_t button_init(void);

/* Snapshot of current button state — safe to read from any task. */
bool     button_is_pressed(void);
uint32_t button_press_count(void);
uint32_t button_last_press_ms_ago(void);
