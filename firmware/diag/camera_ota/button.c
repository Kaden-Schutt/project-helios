/*
 * Button driver + gesture detector.
 *
 * Polls GPIO44 at 100 Hz with a BUTTON_DEBOUNCE_MS debounce, then feeds
 * each press/release edge into a small FSM that emits gesture events.
 * Hold semantics: once a press crosses BUTTON_HOLD_THRESHOLD_MS, we fire
 * HOLD_START (this is when query recording should begin). Any release
 * before that threshold counts as a tap; tap count resolves into a tap
 * gesture after BUTTON_GESTURE_SETTLE_MS of no further presses.
 *
 * Two mixed gestures:
 *   - tap,tap,tap (all short)   -> TRIPLE_TAP
 *   - tap,tap,hold (3rd is long) -> TRIPLE_TAP_HOLD (agent mode)
 * Disambiguated by waiting for HOLD_THRESHOLD on the 3rd press.
 */

#include "button.h"
#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "diag_log.h"

static const char *TAG = "btn";

/* Exposed read-only state. */
static volatile bool     s_pressed = false;
static volatile bool     s_holding = false;
static volatile uint32_t s_count = 0;
static volatile int64_t  s_last_press_us = 0;

static QueueHandle_t s_gesture_q = NULL;

static inline void emit(btn_gesture_t g)
{
    DLOG("[BTN] gesture: %s\n", button_gesture_name(g));
    if (s_gesture_q) {
        (void)xQueueSend(s_gesture_q, &g, 0);
    }
}

/* FSM locals — only touched by button_task. */
typedef enum {
    FSM_IDLE,
    FSM_PRESSED,       /* currently pressed, hold not yet committed */
    FSM_HOLDING,       /* committed to hold — HOLD_START emitted */
    FSM_TAPS_PENDING,  /* released after short press, waiting to see if more taps come */
} fsm_t;

static void resolve_taps(uint8_t taps)
{
    switch (taps) {
    case 1: emit(BTN_GESTURE_SINGLE_TAP); break;
    case 2: emit(BTN_GESTURE_DOUBLE_TAP); break;
    case 3: emit(BTN_GESTURE_TRIPLE_TAP); break;
    case 5: emit(BTN_GESTURE_QUINT_TAP);  break;
    default:
        DLOG("[BTN] %u taps (unassigned gesture)\n", (unsigned)taps);
        break;
    }
}

static void button_task(void *arg)
{
    /* Debouncer locals */
    bool debounced = false;
    bool candidate = false;
    int64_t candidate_since = 0;

    /* FSM locals */
    fsm_t fsm = FSM_IDLE;
    uint8_t tap_count = 0;
    int64_t press_start_us = 0;
    int64_t last_release_us = 0;
    bool long_hold_emitted = false;

    while (1) {
        int raw = gpio_get_level(BUTTON_GPIO);
        bool now_pressed = (raw == 1);
        int64_t now = esp_timer_get_time();

        /* --- Debounce (unchanged behavior) --- */
        bool edge_press = false, edge_release = false;
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
                edge_press = true;
            } else {
                edge_release = true;
            }
        }

        /* --- Gesture FSM --- */
        if (edge_press) {
            press_start_us = now;
            long_hold_emitted = false;
            fsm = FSM_PRESSED;
            /* Keep tap_count as-is so we can detect tap-tap-hold etc. */
        } else if (edge_release) {
            int64_t dur_us = now - press_start_us;
            last_release_us = now;
            if (fsm == FSM_HOLDING) {
                /* Hold committed — what kind of hold? */
                s_holding = false;
                if (tap_count >= 2) {
                    emit(BTN_GESTURE_TRIPLE_TAP_HOLD);
                } else {
                    emit(BTN_GESTURE_HOLD_END);
                }
                tap_count = 0;
                fsm = FSM_IDLE;
            } else {
                /* Short press — tap candidate. */
                (void)dur_us;
                if (tap_count < 255) tap_count++;
                fsm = FSM_TAPS_PENDING;
            }
        }

        /* Check for hold commitment while still pressed. */
        if (fsm == FSM_PRESSED) {
            int64_t held_us = now - press_start_us;
            if (held_us > (BUTTON_HOLD_THRESHOLD_MS * 1000LL)) {
                fsm = FSM_HOLDING;
                s_holding = true;
                if (tap_count >= 2) {
                    /* Agent-mode entry happens on HOLD_START (not release) so
                     * we can distinguish it from plain triple-tap. The HOLD_END
                     * branch will emit TRIPLE_TAP_HOLD on release for the Pi to
                     * pick up; we also emit HOLD_START so any mic-recording
                     * path can still capture audio for the agent handoff. */
                    emit(BTN_GESTURE_HOLD_START);
                } else {
                    emit(BTN_GESTURE_HOLD_START);
                }
            }
        }

        /* Long hold detection (independent of tap count). */
        if (fsm == FSM_HOLDING && !long_hold_emitted) {
            int64_t held_us = now - press_start_us;
            if (held_us > (BUTTON_LONG_HOLD_MS * 1000LL)) {
                long_hold_emitted = true;
                emit(BTN_GESTURE_LONG_HOLD);
                /* Do not reset — user will release normally; HOLD_END will
                 * still fire. This is a "level crossed" notification. */
            }
        }

        /* Resolve pending tap sequence after settle window. */
        if (fsm == FSM_TAPS_PENDING) {
            int64_t since_release = now - last_release_us;
            if (since_release > (BUTTON_GESTURE_SETTLE_MS * 1000LL)) {
                resolve_taps(tap_count);
                tap_count = 0;
                fsm = FSM_IDLE;
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
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    s_gesture_q = xQueueCreate(8, sizeof(btn_gesture_t));
    if (!s_gesture_q) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL);
    DLOG("[CAMDIAG] button_init GPIO=%d pull-down enabled, task=%s\n",
         BUTTON_GPIO, ok == pdPASS ? "up" : "FAILED");
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool     button_is_pressed(void)   { return s_pressed; }
bool     button_is_holding(void)   { return s_holding; }
uint32_t button_press_count(void)  { return s_count; }

uint32_t button_last_press_ms_ago(void)
{
    if (s_last_press_us == 0) return UINT32_MAX;
    int64_t age = (esp_timer_get_time() - s_last_press_us) / 1000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

btn_gesture_t button_gesture_pop(void)
{
    btn_gesture_t g = BTN_GESTURE_NONE;
    if (s_gesture_q) {
        (void)xQueueReceive(s_gesture_q, &g, 0);
    }
    return g;
}

const char *button_gesture_name(btn_gesture_t g)
{
    switch (g) {
    case BTN_GESTURE_NONE:            return "none";
    case BTN_GESTURE_HOLD_START:      return "hold_start";
    case BTN_GESTURE_HOLD_END:        return "hold_end";
    case BTN_GESTURE_SINGLE_TAP:      return "single_tap";
    case BTN_GESTURE_DOUBLE_TAP:      return "double_tap";
    case BTN_GESTURE_TRIPLE_TAP:      return "triple_tap";
    case BTN_GESTURE_QUINT_TAP:       return "quint_tap";
    case BTN_GESTURE_TRIPLE_TAP_HOLD: return "triple_tap_hold";
    case BTN_GESTURE_LONG_HOLD:       return "long_hold";
    default:                          return "?";
    }
}
