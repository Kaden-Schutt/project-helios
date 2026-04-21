/*
 * Button-hold → query client.
 *
 * Flow per button hold:
 *   HOLD_START -> suspend mic_probe, alloc PSRAM record buffer, drain mic
 *   while held -> keep draining mic_helios_read() into buffer (up to QUERY_MAX_S)
 *   HOLD_END   -> resume mic_probe, capture fresh JPEG, POST both to Pi
 *                 (/photo/upload then /query). TTS reply is discarded — the
 *                 Pi plays audio via its own BT sink (pendant has no speaker).
 *
 * We poll button_is_holding() rather than consume gesture events so we
 * don't race with ota.c's /button handler draining the queue.
 */

#include "query_client.h"
#include "button.h"
#include "camera_helios.h"
#include "mic_helios.h"
#include "mic_probe.h"
#include "diag_log.h"

#include <string.h>
#include <inttypes.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "query";

#define CHUNK_BYTES  3200  /* 100 ms of 16 kHz s16le mono */

static esp_err_t http_post(const char *path, const char *content_type,
                           const uint8_t *body, size_t body_len,
                           int timeout_ms)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", HELIOS_PI_URL, path);

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = timeout_ms,
        .buffer_size   = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_http_client_set_header(c, "Content-Type", content_type);
    esp_http_client_set_post_field(c, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int written = (int)esp_http_client_get_content_length(c);
    DLOG("[QUERY] POST %s (%u B) -> %d status=%d content_len=%d\n",
         path, (unsigned)body_len, err, status, written);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) return ESP_FAIL;
    return ESP_OK;
}


static void query_task(void *arg)
{
    const size_t max_bytes = (size_t)QUERY_MAX_S * QUERY_SAMPLE_RATE * 2;
    uint8_t *rec = heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM);
    if (!rec) {
        DLOG("[QUERY] ENOMEM for %zu B PSRAM buffer — task exiting\n", max_bytes);
        vTaskDelete(NULL);
        return;
    }
    DLOG("[QUERY] ready — PSRAM buffer %zu KB, target %s\n",
         max_bytes / 1024, HELIOS_PI_URL);

    while (1) {
        /* Wait for a hold. Poll at 20 Hz — fine for a human trigger. */
        if (!button_is_holding()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        mic_probe_suspend();
        /* Flush any stale DMA samples accumulated while probe was running. */
        mic_helios_start();

        size_t offset = 0;
        size_t underruns = 0;
        while (button_is_holding() && offset + CHUNK_BYTES <= max_bytes) {
            size_t got = 0;
            esp_err_t err = mic_helios_read(rec + offset, CHUNK_BYTES, &got, 200);
            if (err != ESP_OK || got == 0) { underruns++; continue; }
            offset += got;
        }
        mic_probe_resume();

        int64_t t1 = esp_timer_get_time();
        float secs = offset / (float)(QUERY_SAMPLE_RATE * 2);
        DLOG("[QUERY] hold released: %.2fs audio (%zu B, %u underruns) in %lld ms\n",
             secs, offset, (unsigned)underruns, (long long)((t1 - t0) / 1000));

        if (offset < CHUNK_BYTES) {
            DLOG("[QUERY] too short, skipping upload\n");
            continue;
        }

        /* Snap a fresh JPEG. camera_helios_capture() returns a reused
         * framebuffer — grab it, upload, release. */
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        esp_err_t cerr = camera_helios_capture(&jpeg, &jpeg_len);
        if (cerr == ESP_OK && jpeg && jpeg_len > 0) {
            http_post("/photo/upload", "image/jpeg", jpeg, jpeg_len, 10000);
            camera_helios_return_fb();
        } else {
            DLOG("[QUERY] camera capture failed 0x%x — sending audio only\n", cerr);
        }

        /* POST audio. Pi consumes this as raw PCM s16le 16 kHz when the
         * content-type isn't JSON. TTS reply is fire-and-forget. */
        http_post("/query", "audio/L16", rec, offset, 30000);

        int64_t t2 = esp_timer_get_time();
        DLOG("[QUERY] round-trip %lld ms total\n", (long long)((t2 - t0) / 1000));
    }
}


esp_err_t query_client_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        query_task, "query", 6144, NULL, 4, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
