/*
 * Helios Firmware — Peripheral Test
 * ==================================
 * Minimal test firmware for XIAO ESP32S3 Sense.
 * Initializes camera + mic, captures one frame and a short audio clip,
 * dumps stats over USB serial. No WiFi, no speaker — just hardware validation.
 *
 * Build:  idf.py build
 * Flash:  idf.py -p /dev/cu.usbmodem1101 flash monitor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "mbedtls/base64.h"
#include "driver/gpio.h"

#include "helios.h"

static const char *TAG = "helios";

// Button GPIO — user will wire a physical button here
// Change this to whichever GPIO the button is wired to
#define BUTTON_GPIO     GPIO_NUM_1   // D0 on XIAO header
#define BUTTON_ACTIVE   1            // Active high (button module with pull-down)

static void print_sysinfo(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("\n");
    printf("========================================\n");
    printf("  HELIOS PERIPHERAL TEST\n");
    printf("========================================\n");
    printf("  Chip:    ESP32-S3 rev %d.%d, %d cores @ 240MHz\n",
           chip.revision / 100, chip.revision % 100, chip.cores);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    printf("  Flash:   %lu MB\n", (unsigned long)(flash_size / (1024 * 1024)));
    printf("  PSRAM:   %lu KB free\n", (unsigned long)(esp_psram_get_size() / 1024));
    printf("  Heap:    %lu KB free\n", (unsigned long)(esp_get_free_heap_size() / 1024));
    printf("  Button:  GPIO %d (active %s)\n", BUTTON_GPIO, BUTTON_ACTIVE ? "high" : "low");
    printf("========================================\n\n");
}

static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Module has its own pull-down
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "button on GPIO %d ready (pull-up, active low)", BUTTON_GPIO);
}

static bool button_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE;
}

static void test_camera(void)
{
    ESP_LOGI(TAG, "--- CAMERA TEST ---");

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;

    esp_err_t err = camera_capture_jpeg(&jpeg_buf, &jpeg_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA TEST FAILED");
        return;
    }

    // Print JPEG header bytes for sanity check (should start with FF D8)
    if (jpeg_len >= 2) {
        printf("  JPEG header: %02X %02X (expect FF D8)\n", jpeg_buf[0], jpeg_buf[1]);
    }
    printf("  JPEG size:   %zu bytes (%.1f KB)\n", jpeg_len, jpeg_len / 1024.0f);
    printf("  CAMERA TEST: PASS\n\n");

    camera_return_fb();
}

static void test_mic(void)
{
    ESP_LOGI(TAG, "--- MIC TEST (1 second) ---");

    uint8_t *pcm_buf = NULL;
    size_t pcm_len = 0;

    esp_err_t err = mic_record(1000, &pcm_buf, &pcm_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MIC TEST FAILED");
        return;
    }

    // Check if we got actual audio (not all zeros)
    int16_t *samples = (int16_t *)pcm_buf;
    size_t n_samples = pcm_len / 2;
    int32_t peak = 0;
    int64_t sum = 0;

    for (size_t i = 0; i < n_samples; i++) {
        int32_t val = samples[i] < 0 ? -samples[i] : samples[i];
        if (val > peak) peak = val;
        sum += val;
    }

    int32_t avg = (int32_t)(sum / n_samples);

    printf("  PCM size:    %zu bytes (%zu samples)\n", pcm_len, n_samples);
    printf("  Peak level:  %ld / 32767\n", (long)peak);
    printf("  Avg level:   %ld\n", (long)avg);

    if (peak > 100) {
        printf("  MIC TEST:    PASS (signal detected)\n\n");
    } else {
        printf("  MIC TEST:    WARN (very low signal — check mic connection)\n\n");
    }

    mic_free_buf(pcm_buf);
}

void app_main(void)
{
    print_sysinfo();

    // Init peripherals
    ESP_LOGI(TAG, "Initializing camera...");
    esp_err_t cam_ok = camera_init();

    ESP_LOGI(TAG, "Initializing microphone...");
    esp_err_t mic_ok = mic_init();

    // Init button
    button_init();

    printf("\n");
    printf("  Camera: %s\n", cam_ok == ESP_OK ? "OK" : "FAILED");
    printf("  Mic:    %s\n", mic_ok == ESP_OK ? "OK" : "FAILED");
    printf("\n");

    // Run peripheral tests once on boot
    if (cam_ok == ESP_OK) {
        test_camera();
    }
    if (mic_ok == ESP_OK) {
        test_mic();
    }

    // Print heap after init
    printf("  Heap after init: %lu KB free\n\n", (unsigned long)(esp_get_free_heap_size() / 1024));

    // Main loop — button press triggers capture + record
    printf("  Press button (GPIO %d) to capture photo + record 2s audio\n", BUTTON_GPIO);
    printf("  Output is printed over USB serial.\n\n");

    while (1) {
        if (button_pressed()) {
            // Debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!button_pressed()) {
                continue;  // glitch
            }

            printf("\n>>> BUTTON PRESSED — capturing...\n\n");

            uint8_t *jpeg_buf = NULL;
            size_t jpeg_len = 0;
            uint8_t *pcm_buf = NULL;
            size_t pcm_len = 0;

            // Camera capture
            if (cam_ok == ESP_OK) {
                if (camera_capture_jpeg(&jpeg_buf, &jpeg_len) == ESP_OK) {
                    printf("  Photo: %zu bytes JPEG (%.1f KB)\n", jpeg_len, jpeg_len / 1024.0f);
                }
            }

            // Mic record
            if (mic_ok == ESP_OK) {
                printf("  Recording 2s...\n");
                mic_record(2000, &pcm_buf, &pcm_len);
            }

            printf("  Heap: %lu KB free\n", (unsigned long)(esp_get_free_heap_size() / 1024));

            // Send data as base64 text lines — avoids binary-over-serial issues
            //   HELIOS_JPEG:<base64 encoded jpeg>\n
            //   HELIOS_PCM:<base64 encoded pcm>\n
            //   HELIOS_END\n
            if (jpeg_buf && jpeg_len > 0) {
                // Base64 encode JPEG
                size_t b64_len = 0;
                unsigned char *b64_buf = NULL;
                mbedtls_base64_encode(NULL, 0, &b64_len, jpeg_buf, jpeg_len);
                b64_buf = malloc(b64_len);
                if (b64_buf) {
                    mbedtls_base64_encode(b64_buf, b64_len, &b64_len, jpeg_buf, jpeg_len);
                    printf("HELIOS_JPEG:");
                    fwrite(b64_buf, 1, b64_len, stdout);
                    printf("\n");
                    fflush(stdout);
                    free(b64_buf);
                }

                // Base64 encode PCM
                if (pcm_buf && pcm_len > 0) {
                    b64_len = 0;
                    mbedtls_base64_encode(NULL, 0, &b64_len, pcm_buf, pcm_len);
                    b64_buf = malloc(b64_len);
                    if (b64_buf) {
                        mbedtls_base64_encode(b64_buf, b64_len, &b64_len, pcm_buf, pcm_len);
                        printf("HELIOS_PCM:");
                        fwrite(b64_buf, 1, b64_len, stdout);
                        printf("\n");
                        fflush(stdout);
                        free(b64_buf);
                    }
                }

                printf("HELIOS_END\n");
                fflush(stdout);
            }

            if (jpeg_buf) camera_return_fb();
            if (pcm_buf) mic_free_buf(pcm_buf);

            printf(">>> DONE\n");

            // Wait for button release
            while (button_pressed()) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(200));  // debounce on release
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // poll at 50Hz
    }
}
