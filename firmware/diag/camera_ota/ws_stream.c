/*
 * Always-on mic streamer — reads PDM PCM from mic_helios in 100ms chunks and
 * pushes them as binary WebSocket frames to the Pi's /ws/stream endpoint.
 *
 * Pairs with wake_stream.py on the Pi side. The Pi is responsible for wake
 * detection + endpointing; pendant is a dumb audio firehose. Future tweak:
 * an on-pendant RMS gate to cut STT cost and WiFi power when the user is
 * silent, emitting brief "still alive" keepalives instead of full audio.
 *
 * Thread layout:
 *   core 1  mic streamer task (this file)
 *   core 1  PDM probe task (mic_probe.c) — unchanged, keeps RMS for /mic
 *   core 0  WiFi + HTTP server (esp-idf default)
 *
 * Both mic consumers call mic_helios_read(), so i2s_channel_read() is the
 * backing primitive. It's safe to call from multiple tasks BUT each call
 * drains samples from the shared DMA ring. Parallel consumption means each
 * task sees only half the audio. Solution: the mic-probe task's RMS is a
 * rough liveness indicator and can tolerate holes; the streamer task gets
 * priority access. If this becomes a problem we'll fan out via a ring buffer.
 */

#include "ws_stream.h"
#include "mic_helios.h"
#include "diag_log.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws_stream";

#define CHUNK_BYTES   3200   /* 100 ms of 16 kHz s16le mono */
#define TASK_STACK    4096
#define TASK_PRIO     4
#define TASK_CORE     1

static esp_websocket_client_handle_t s_client = NULL;
static TaskHandle_t    s_task = NULL;
static volatile bool   s_run = false;
static volatile bool   s_suspended = false;
static volatile bool   s_connected = false;
static volatile uint32_t s_bytes_sent = 0;


static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)base; (void)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        DLOG("[WS-STREAM] connected\n");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        DLOG("[WS-STREAM] disconnected\n");
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_connected = false;
        DLOG("[WS-STREAM] error event\n");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        s_connected = false;
        DLOG("[WS-STREAM] closed\n");
        break;
    default:
        break;
    }
}


static void ws_stream_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL);
    if (!buf) {
        DLOG("[WS-STREAM] ENOMEM — task exiting\n");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_run) {
        if (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t got = 0;
        esp_err_t err = mic_helios_read(buf, CHUNK_BYTES, &got, 200);
        if (err != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!s_client || !s_connected) {
            /* Drop audio while we're not connected — don't buffer, the user's
             * words a few seconds ago are no longer relevant anyway. */
            continue;
        }

        int sent = esp_websocket_client_send_bin(
            s_client, (const char *)buf, got, pdMS_TO_TICKS(200));
        if (sent < 0) {
            DLOG("[WS-STREAM] send rc=%d\n", sent);
        } else {
            s_bytes_sent += (uint32_t)sent;
        }
    }

    heap_caps_free(buf);
    s_task = NULL;
    vTaskDelete(NULL);
}


esp_err_t ws_stream_start(const char *uri)
{
    if (s_run) return ESP_OK;
    if (!uri || !*uri) uri = HELIOS_STREAM_URI;

    esp_websocket_client_config_t cfg = {
        .uri                   = uri,
        .reconnect_timeout_ms  = 2000,
        .network_timeout_ms    = 10000,
        .buffer_size           = 4096,
        .disable_auto_reconnect = false,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        DLOG("[WS-STREAM] client_init FAILED\n");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        DLOG("[WS-STREAM] client_start FAILED 0x%x\n", err);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_run = true;
    s_suspended = false;
    BaseType_t ok = xTaskCreatePinnedToCore(
        ws_stream_task, "ws_stream", TASK_STACK, NULL,
        TASK_PRIO, &s_task, TASK_CORE);
    if (ok != pdPASS) {
        s_run = false;
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    DLOG("[WS-STREAM] started -> %s\n", uri);
    return ESP_OK;
}


esp_err_t ws_stream_stop(void)
{
    s_run = false;
    /* Wait up to 300ms for task to exit. */
    for (int i = 0; i < 30 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));

    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    DLOG("[WS-STREAM] stopped\n");
    return ESP_OK;
}


void ws_stream_suspend(void) { s_suspended = true;  DLOG("[WS-STREAM] suspended\n"); }
void ws_stream_resume(void)  { s_suspended = false; DLOG("[WS-STREAM] resumed\n");  }

bool     ws_stream_is_connected(void) { return s_connected; }
uint32_t ws_stream_bytes_sent(void)   { return s_bytes_sent; }
