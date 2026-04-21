#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BUTTON_GPIO                44    /* D7 on the XIAO Sense, active-high (no SD pullup conflict) */
#define BUTTON_DEBOUNCE_MS         30

/* Gesture timing (see project_helios_modes memory for the UX contract). */
#define BUTTON_HOLD_THRESHOLD_MS   250   /* press > this = committed to hold (starts query) */
#define BUTTON_GESTURE_SETTLE_MS   350   /* after last release with no new press = resolve tap count */
#define BUTTON_LONG_HOLD_MS        3000  /* privacy-pause trigger */

typedef enum {
    BTN_GESTURE_NONE = 0,
    BTN_GESTURE_HOLD_START,        /* press crossed HOLD_THRESHOLD — start recording */
    BTN_GESTURE_HOLD_END,          /* hold released — stop recording, send query */
    BTN_GESTURE_SINGLE_TAP,        /* short press, no follow-ups within SETTLE */
    BTN_GESTURE_DOUBLE_TAP,        /* two short presses within SETTLE */
    BTN_GESTURE_TRIPLE_TAP,        /* three short presses — settings toggle */
    BTN_GESTURE_QUINT_TAP,         /* five short presses — sleep toggle */
    BTN_GESTURE_TRIPLE_TAP_HOLD,   /* two taps then a hold on the 3rd — agent mode enter */
    BTN_GESTURE_LONG_HOLD,         /* single press held past LONG_HOLD_MS — privacy pause */
} btn_gesture_t;

esp_err_t button_init(void);

/* Instantaneous debounced state. */
bool     button_is_pressed(void);

/* True once current press has crossed HOLD_THRESHOLD (i.e. HOLD_START fired and HOLD_END hasn't). */
bool     button_is_holding(void);

uint32_t button_press_count(void);
uint32_t button_last_press_ms_ago(void);

/* Pop the next pending gesture event; returns BTN_GESTURE_NONE if queue empty.
 * Safe to call from any task. */
btn_gesture_t button_gesture_pop(void);

/* Human-readable name for logging/HTTP surface. */
const char *button_gesture_name(btn_gesture_t g);
