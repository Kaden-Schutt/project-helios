/*
 * Helios Firmware — BLE Voice + Vision Assistant
 * ================================================
 * Button hold → camera JPEG + mic Opus stream over BLE
 * Pi runs STT→LLM(vision)→TTS → BLE sends Opus TTS back
 * Speaker plays TTS on Core 1 (non-blocking)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "helios.h"

static const char *TAG = "helios";

#define BUTTON_GPIO     GPIO_NUM_4

// TTS receive buffer (256KB in PSRAM)
#define TTS_BUF_SIZE    (256 * 1024)
static uint8_t *tts_buf = NULL;
static volatile size_t tts_received = 0;
static volatile size_t tts_expected = 0;
static volatile bool tts_done = false;
static SemaphoreHandle_t tts_sem = NULL;
static SemaphoreHandle_t spk_done_sem = NULL;  // signalled when speaker_task finishes

// Config
static helios_config_t cfg;
static int button_idle_level = 0;

// --- Button ---
static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static bool button_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) != button_idle_level;
}

// --- Speaker task (runs on Core 1) ---
typedef struct {
    uint8_t *data;
    size_t len;
    int sample_rate;
} spk_args_t;

static void speaker_task(void *arg)
{
    spk_args_t *a = (spk_args_t *)arg;
    ESP_LOGI(TAG, "Speaker task: playing %zu bytes Opus on Core 1", a->len);
    speaker_play_opus(a->data, a->len, a->sample_rate);
    ble_notify_control(BLE_CMD_PLAYBACK_DONE, NULL, 0);
    ESP_LOGI(TAG, "Playback done");
    free(a);
    xSemaphoreGive(spk_done_sem);  // signal: safe to reuse tts_buf
    vTaskDelete(NULL);
}

// --- BLE Callbacks ---
static void on_tts_chunk(const uint8_t *data, size_t len, bool is_first, bool is_last)
{
    if (is_first && len >= 4) {
        tts_expected = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        tts_received = 0;
        tts_done = false;
        ESP_LOGI(TAG, "TTS incoming: %zu bytes", tts_expected);
        data += 4;
        len -= 4;
    }

    if (is_last) {
        if (!tts_done) {
            tts_done = true;
            xSemaphoreGive(tts_sem);
        }
        return;
    }

    if (tts_buf && len > 0 && tts_received + len <= TTS_BUF_SIZE) {
        memcpy(tts_buf + tts_received, data, len);
        tts_received += len;
    }

    if (!tts_done && tts_received >= tts_expected && tts_expected > 0) {
        tts_done = true;
        xSemaphoreGive(tts_sem);
    }
}

static void on_control(uint8_t cmd, const uint8_t *payload, size_t payload_len)
{
    switch (cmd) {
    case BLE_CMD_CONNECTED:
        ESP_LOGI(TAG, "Pi connected");
        break;
    case BLE_CMD_PROCESSING:
        ESP_LOGI(TAG, "Pi processing...");
        break;
    case BLE_CMD_SET_VOLUME:
        if (payload_len >= 1) {
            speaker_set_volume(payload[0]);
            cfg.speaker_volume = payload[0];
            config_save(&cfg);
        }
        break;
    case BLE_CMD_ERROR:
        ESP_LOGW(TAG, "Pi error: %.*s", (int)payload_len, (const char *)payload);
        break;
    }
}

void app_main(void)
{
    printf("\n========================================\n");
    printf("  HELIOS — BLE Voice + Vision Assistant\n");
    printf("========================================\n\n");

    // Init peripherals
    ESP_LOGI(TAG, "Initializing mic...");
    esp_err_t mic_ok = mic_init();

    ESP_LOGI(TAG, "Initializing speaker...");
    esp_err_t spk_ok = speaker_init();

    ESP_LOGI(TAG, "Initializing SD card...");
    esp_err_t sd_ok = sdcard_init();

    ESP_LOGI(TAG, "Initializing camera...");
    esp_err_t cam_ok = camera_init();

    // Load config
    if (sd_ok == ESP_OK) {
        config_load(&cfg);
    } else {
        config_defaults(&cfg);
    }
    if (spk_ok == ESP_OK) speaker_set_volume(cfg.speaker_volume);

    button_init();

    // Init BLE
    tts_sem = xSemaphoreCreateBinary();
    spk_done_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(spk_done_sem);  // start as "available" (no speaker running)
    tts_buf = heap_caps_malloc(TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Initializing BLE...");
    esp_err_t ble_ok = ble_init(on_tts_chunk, on_control);

    printf("  Mic:     %s\n", mic_ok == ESP_OK ? "OK" : "FAILED");
    printf("  Speaker: %s\n", spk_ok == ESP_OK ? "OK" : "FAILED");
    printf("  SD Card: %s\n", sd_ok == ESP_OK ? "OK" : "FAILED");
    printf("  Camera:  %s\n", cam_ok == ESP_OK ? "OK" : "FAILED");
    printf("  BLE:     %s\n", ble_ok == ESP_OK ? "OK" : "FAILED");
    printf("  TTS buf: %s\n", tts_buf ? "OK (256KB PSRAM)" : "FAILED");
    printf("  Volume:  %d%%\n", cfg.speaker_volume);
    printf("  Heap:    %lu KB free\n\n", (unsigned long)(esp_get_free_heap_size() / 1024));

    // Quick mic test
    if (mic_ok == ESP_OK) {
        uint8_t *test_pcm = NULL;
        size_t test_len = 0;
        if (mic_record(500, &test_pcm, &test_len) == ESP_OK) {
            int16_t *samples = (int16_t *)test_pcm;
            int32_t peak = 0;
            for (size_t i = 0; i < test_len / 2; i++) {
                int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak) peak = v;
            }
            printf("  Mic test: peak=%ld %s\n", (long)peak, peak > 100 ? "OK" : "WARN");
            mic_free_buf(test_pcm);
        }
    }

    // Auto-detect button idle level (or use saved)
    if (cfg.button_idle_level >= 0) {
        button_idle_level = cfg.button_idle_level;
        ESP_LOGI(TAG, "Button idle level from config: %d", button_idle_level);
    } else {
        ESP_LOGI(TAG, "Auto-detecting button...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        int high = 0;
        for (int i = 0; i < 10; i++) {
            high += gpio_get_level(BUTTON_GPIO);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        button_idle_level = (high >= 5) ? 1 : 0;
        cfg.button_idle_level = button_idle_level;
        if (sd_ok == ESP_OK) config_save(&cfg);
    }

    printf("\n  Waiting for BLE connection...\n");
    while (!ble_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("  Connected! Hold button to talk.\n\n");

    // --- Main voice loop ---
    while (1) {
        if (!ble_is_connected()) {
            printf("  BLE disconnected. Waiting...\n");
            while (!ble_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
            printf("  Reconnected!\n\n");
        }

        if (button_pressed()) {
            // Debounce
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!button_pressed()) goto poll;

            ESP_LOGI(TAG, "=== QUERY START ===");

            // Wait for any previous speaker playback to finish before
            // touching tts_buf (prevents corruption if user presses
            // button while TTS is still playing). Must block until
            // done — tts_buf is shared, no safe timeout.
            xSemaphoreTake(spk_done_sem, portMAX_DELAY);

            // Reset TTS state BEFORE any BLE communication —
            // the Pi can't send TTS until it receives our mic data,
            // so this is safe. Resetting after button release is NOT
            // safe because the Pi might respond before we reset.
            tts_received = 0;
            tts_expected = 0;
            tts_done = false;
            // Drain any stale semaphore signal from previous query
            // (on_tts_chunk could have given it at size threshold)
            xSemaphoreTake(tts_sem, 0);

            // 1. Notify Pi: button pressed
            ble_notify_control(BLE_CMD_BUTTON_PRESSED, NULL, 0);

            // 2. Camera JPEG capture + send (~130ms)
            if (cam_ok == ESP_OK) {
                uint8_t *jpeg_buf = NULL;
                size_t jpeg_len = 0;
                if (camera_capture_jpeg(&jpeg_buf, &jpeg_len) == ESP_OK) {
                    ESP_LOGI(TAG, "JPEG: %zu bytes, sending...", jpeg_len);
                    ble_send_jpeg(jpeg_buf, jpeg_len);
                    camera_return_fb();
                } else {
                    ESP_LOGW(TAG, "Camera capture failed");
                }
            }

            // 3. Stream mic Opus until button released
            ESP_LOGI(TAG, "LISTENING...");
            ble_stream_mic_opus(button_pressed, 30000);

            // 4. Notify Pi: button released
            ble_notify_control(BLE_CMD_BUTTON_RELEASED, NULL, 0);

            // 5. Wait for TTS response
            ESP_LOGI(TAG, "Waiting for TTS...");
            if (xSemaphoreTake(tts_sem, pdMS_TO_TICKS(60000)) == pdTRUE && tts_received > 0) {
                ESP_LOGI(TAG, "TTS received: %zu bytes, spawning speaker on Core 1", tts_received);

                // 6. Play on Core 1 (non-blocking)
                spk_args_t *args = malloc(sizeof(spk_args_t));
                if (args) {
                    args->data = tts_buf;
                    args->len = tts_received;
                    args->sample_rate = SPK_SAMPLE_RATE;
                    xTaskCreatePinnedToCore(speaker_task, "spk", 8192,
                                            args, 5, NULL, 1);
                } else {
                    xSemaphoreGive(spk_done_sem);  // no task spawned
                }
            } else {
                ESP_LOGW(TAG, "No TTS received (timeout)");
                xSemaphoreGive(spk_done_sem);  // no playback needed
            }

            // Wait for button release
            while (button_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(200));
        }

poll:
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
