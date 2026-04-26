/*
 * WiFi throughput test for ESP32-S3
 *
 * Connects to WiFi, then runs:
 *   - DOWNLOAD: 3x HTTP GET of 1MB, measure Mbps
 *   - UPLOAD:   3x HTTP POST of 1MB, measure Mbps
 *
 * Server: python3 throughput_server.py (running on SERVER_HOST)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "wifi_helios.h"
#include "wifi_credentials.h"

static const char *TAG = "throughput";

#define TEST_BYTES          (1024 * 1024)   // 1 MB
#define TEST_ITERATIONS     3
#define HTTP_BUFFER_SIZE    8192

// --- Download test ---

typedef struct {
    int64_t bytes_rx;
} dl_ctx_t;

static esp_err_t dl_event_handler(esp_http_client_event_t *evt)
{
    dl_ctx_t *ctx = (dl_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        ctx->bytes_rx += evt->data_len;
    }
    return ESP_OK;
}

static void test_download(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/download/%d",
             SERVER_HOST, SERVER_PORT, TEST_BYTES);

    ESP_LOGI(TAG, "==== DOWNLOAD TEST ====");
    ESP_LOGI(TAG, "URL: %s", url);

    double total_mbps = 0;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        dl_ctx_t ctx = { .bytes_rx = 0 };
        esp_http_client_config_t cfg = {
            .url            = url,
            .event_handler  = dl_event_handler,
            .user_data      = &ctx,
            .buffer_size    = HTTP_BUFFER_SIZE,
            .timeout_ms     = 30000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(client);
        int64_t t1 = esp_timer_get_time();
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  iter %d: error %s", i + 1, esp_err_to_name(err));
            continue;
        }

        double sec  = (t1 - t0) / 1e6;
        double mbps = (ctx.bytes_rx * 8.0) / (1e6 * sec);
        double mBps = ctx.bytes_rx / (1024.0 * 1024.0 * sec);
        total_mbps += mbps;
        ESP_LOGI(TAG, "  iter %d: status=%d  %lld bytes in %.3f sec  %.2f Mbps  (%.2f MB/s)",
                 i + 1, status, (long long)ctx.bytes_rx, sec, mbps, mBps);
    }
    ESP_LOGI(TAG, "  avg: %.2f Mbps", total_mbps / TEST_ITERATIONS);
}

// --- Upload test ---

static void test_upload(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/upload", SERVER_HOST, SERVER_PORT);

    ESP_LOGI(TAG, "==== UPLOAD TEST ====");
    ESP_LOGI(TAG, "URL: %s", url);

    // Allocate payload in PSRAM
    uint8_t *payload = heap_caps_malloc(TEST_BYTES, MALLOC_CAP_SPIRAM);
    if (!payload) {
        ESP_LOGE(TAG, "failed to alloc %d bytes in PSRAM", TEST_BYTES);
        return;
    }
    for (int i = 0; i < TEST_BYTES; i++) payload[i] = (uint8_t)i;

    double total_mbps = 0;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        esp_http_client_config_t cfg = {
            .url            = url,
            .method         = HTTP_METHOD_POST,
            .buffer_size    = HTTP_BUFFER_SIZE,
            .timeout_ms     = 30000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        esp_http_client_set_post_field(client, (const char *)payload, TEST_BYTES);

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(client);
        int64_t t1 = esp_timer_get_time();
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  iter %d: error %s", i + 1, esp_err_to_name(err));
            continue;
        }

        double sec  = (t1 - t0) / 1e6;
        double mbps = (TEST_BYTES * 8.0) / (1e6 * sec);
        double mBps = TEST_BYTES / (1024.0 * 1024.0 * sec);
        total_mbps += mbps;
        ESP_LOGI(TAG, "  iter %d: status=%d  %d bytes in %.3f sec  %.2f Mbps  (%.2f MB/s)",
                 i + 1, status, TEST_BYTES, sec, mbps, mBps);
    }
    ESP_LOGI(TAG, "  avg: %.2f Mbps", total_mbps / TEST_ITERATIONS);

    free(payload);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Helios WiFi Throughput Test ===");

    // Connect
    esp_err_t err = wifi_helios_init(WIFI_SSID, WIFI_PASSWORD, 30000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_helios_init failed: %s", esp_err_to_name(err));
        return;
    }
    char ip[16];
    if (wifi_helios_get_ip(ip, sizeof(ip)) == ESP_OK) {
        ESP_LOGI(TAG, "connected as %s", ip);
    }

    // Let things settle
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Run tests in a loop so we can see variability
    while (1) {
        test_download();
        vTaskDelay(pdMS_TO_TICKS(1000));
        test_upload();
        ESP_LOGI(TAG, "sleeping 10s before next round...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
