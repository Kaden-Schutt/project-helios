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
#include "esp_cache.h"

static const char *TAG = "helios";

#define BUTTON_GPIO     GPIO_NUM_4

// TTS receive buffer (256KB in PSRAM)
#define TTS_BUF_SIZE    (256 * 1024)
#define TTS_WATERMARK   900   // ~300ms of Opus at 24kbps before playback starts
static uint8_t *tts_buf = NULL;
static volatile size_t tts_received = 0;
static volatile bool tts_done = false;
static volatile bool tts_watermark_given = false;
static SemaphoreHandle_t tts_sem = NULL;
static SemaphoreHandle_t tts_watermark_sem = NULL;
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

// --- Speaker task (runs on Core 1, owns full playback lifecycle) ---
// Waits for watermark, inits speaker, streams Opus from PSRAM buffer,
// deinits speaker, re-inits camera+mic, signals spk_done_sem.

static void speaker_task(void *arg)
{
    int sample_rate = (int)(intptr_t)arg;

    // Wait for enough data to start playback
    ESP_LOGI(TAG, "Speaker task: waiting for watermark (%d bytes)...", TTS_WATERMARK);
    if (xSemaphoreTake(tts_watermark_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS watermark timeout — no audio received");
        goto cleanup;
    }

    // Init speaker (camera+mic already deinit'd by main loop)
    esp_err_t err = speaker_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker_init failed: 0x%x", err);
        goto cleanup;
    }
    speaker_set_volume(cfg.speaker_volume);

    ESP_LOGI(TAG, "Streaming playback started (%zu bytes buffered)", tts_received);
    speaker_play_opus_stream(tts_buf, TTS_BUF_SIZE,
                              &tts_received, &tts_done, sample_rate);
    ble_notify_control(BLE_CMD_PLAYBACK_DONE, NULL, 0);
    ESP_LOGI(TAG, "Playback done");

cleanup:
    speaker_deinit();
    ESP_LOGI(TAG, "Restoring camera + mic...");
    camera_init();
    mic_init();
    xSemaphoreGive(spk_done_sem);
    vTaskDelete(NULL);
}

// --- BLE Callbacks ---
static void on_tts_chunk(const uint8_t *data, size_t len, bool is_first, bool is_last)
{
    // Start marker: 0xFFFFFFFF (mirrors mic→Pi Opus protocol)
    if (is_first && len >= 4 &&
        data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[3] == 0xFF) {
        tts_received = 0;
        tts_done = false;
        tts_watermark_given = false;
        xSemaphoreTake(tts_watermark_sem, 0);  // drain any stale signal
        ESP_LOGI(TAG, "TTS stream started");
        data += 4;
        len -= 4;
    }

    // End marker: empty write
    if (is_last) {
        if (!tts_done) {
            tts_done = true;
            if (tts_buf && tts_received > 0) {
                size_t flush_size = (tts_received + 31) & ~31;
                if (flush_size > TTS_BUF_SIZE) flush_size = TTS_BUF_SIZE;
                esp_cache_msync(tts_buf, flush_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
            // Wake speaker task if watermark was never reached (short response)
            if (!tts_watermark_given) {
                tts_watermark_given = true;
                xSemaphoreGive(tts_watermark_sem);
            }
            xSemaphoreGive(tts_sem);
        }
        return;
    }

    // Append data to PSRAM buffer
    if (tts_buf && len > 0 && tts_received + len <= TTS_BUF_SIZE) {
        memcpy(tts_buf + tts_received, data, len);
        tts_received += len;

        // Flush cache after every write for streaming coherence
        size_t flush_start = (tts_received - len) & ~31;
        size_t flush_end = (tts_received + 31) & ~31;
        if (flush_end > TTS_BUF_SIZE) flush_end = TTS_BUF_SIZE;
        esp_cache_msync(tts_buf + flush_start, flush_end - flush_start,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        // Watermark: signal speaker task to start
        if (!tts_watermark_given && tts_received >= TTS_WATERMARK) {
            tts_watermark_given = true;
            xSemaphoreGive(tts_watermark_sem);
            ESP_LOGI(TAG, "TTS watermark reached (%zu bytes)", tts_received);
        }
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

    // Init peripherals (speaker deferred — only init'd during playback)
    ESP_LOGI(TAG, "Initializing mic...");
    esp_err_t mic_ok = mic_init();

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

    button_init();

    // Init BLE
    tts_sem = xSemaphoreCreateBinary();
    tts_watermark_sem = xSemaphoreCreateBinary();
    spk_done_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(spk_done_sem);  // start as "available" (no speaker running)
    tts_buf = heap_caps_aligned_alloc(32, TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Initializing BLE...");
    esp_err_t ble_ok = ble_init(on_tts_chunk, on_control);

    printf("  Mic:     %s\n", mic_ok == ESP_OK ? "OK" : "FAILED");
    printf("  Speaker: deferred (init during playback)\n");
    printf("  SD Card: %s\n", sd_ok == ESP_OK ? "OK" : "FAILED");
    printf("  Camera:  %s\n", cam_ok == ESP_OK ? "OK" : "FAILED");
    printf("  BLE:     %s\n", ble_ok == ESP_OK ? "OK" : "FAILED");
    printf("  TTS buf: %s\n", tts_buf ? "OK (256KB PSRAM)" : "FAILED");
    printf("  Volume:  %d%%\n", cfg.speaker_volume);
    printf("  Heap:    %lu KB free\n", (unsigned long)(esp_get_free_heap_size() / 1024));

    // Button idle level: use saved or sample now (no delay)
    if (cfg.button_idle_level >= 0) {
        button_idle_level = cfg.button_idle_level;
    } else {
        int high = 0;
        for (int i = 0; i < 10; i++) {
            high += gpio_get_level(BUTTON_GPIO);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        button_idle_level = (high >= 5) ? 1 : 0;
        cfg.button_idle_level = button_idle_level;
        if (sd_ok == ESP_OK) config_save(&cfg);
    }

    printf("  Waiting for BLE...\n");
    while (!ble_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("  Ready.\n\n");

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
            tts_done = false;
            tts_watermark_given = false;
            // Drain any stale semaphore signals from previous query
            xSemaphoreTake(tts_sem, 0);
            xSemaphoreTake(tts_watermark_sem, 0);

            // 1. Notify Pi: button pressed
            ble_notify_control(BLE_CMD_BUTTON_PRESSED, NULL, 0);

            // 2. Camera JPEG capture + send (~130ms)
            {
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
            esp_err_t mic_rc = ble_stream_mic_opus(button_pressed, 30000);
            if (mic_rc != ESP_OK) {
                ESP_LOGE(TAG, "Mic stream failed: 0x%x", mic_rc);
            }

            // 4. Notify Pi: button released
            ble_notify_control(BLE_CMD_BUTTON_RELEASED, NULL, 0);

            // 5. Kill camera + mic (free power rail noise sources)
            camera_deinit();
            mic_deinit();

            // 6. Spawn speaker task on Core 1
            // Task handles: wait for watermark → speaker_init → streaming playback
            //   → speaker_deinit → camera_init + mic_init → spk_done_sem
            ESP_LOGI(TAG, "Spawning speaker task...");
            xTaskCreatePinnedToCore(speaker_task, "spk", 32768,
                                    (void *)(intptr_t)SPK_SAMPLE_RATE, 5, NULL, 0);

            // Wait for button release
            while (button_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(200));
        }

poll:
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
