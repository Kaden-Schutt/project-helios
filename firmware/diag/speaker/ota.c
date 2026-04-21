/*
 * Minimal OTA HTTP server for the speaker diagnostic.
 *
 *   curl --data-binary @firmware.bin http://helios-diag.local/ota
 *
 * The body is written to the currently-idle OTA partition (ota_0/ota_1),
 * validated via esp_ota_end, boot partition is updated, and the device
 * reboots into the new image. One endpoint, no auth — LAN-only tool.
 *
 * GET /info returns a small JSON status so you can confirm the server
 * is alive before sending a firmware blob.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diag_log.h"

static const char *TAG = "ota";
static httpd_handle_t s_server = NULL;

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    static char buf[16384];
    int n = dlog_dump(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    char body[384];
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"app_name\": \"%s\",\n"
        "  \"app_version\": \"%s\",\n"
        "  \"compile_time\": \"%s %s\",\n"
        "  \"idf_version\": \"%s\",\n"
        "  \"running_partition\": \"%s\",\n"
        "  \"boot_partition\": \"%s\",\n"
        "  \"uptime_s\": %lld\n"
        "}\n",
        app->project_name, app->version, app->date, app->time,
        app->idf_ver,
        running ? running->label : "?",
        boot ? boot->label : "?",
        (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA POST: content-length=%d", req->content_len);
    printf("[OTA] incoming upload, %d bytes expected\n", req->content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "writing to %s (offset=0x%lx size=0x%lx)",
             update->label, update->address, update->size);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char buf[2048];
    int remaining = req->content_len;
    int total = 0;
    int64_t t0 = esp_timer_get_time();
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                esp_err_to_name(err));
            return ESP_FAIL;
        }
        total += got;
        remaining -= got;
        if ((total & 0xFFFF) == 0) {
            ESP_LOGI(TAG, "progress %d / %d", total, req->content_len);
        }
    }
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "received %d bytes in %lld ms", total, dt / 1000);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    const char *ok =
        "OK — flashed, rebooting in 1s\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ok, strlen(ok));

    /* Give the response time to flush before we yank the net stack. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "rebooting into new image");
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size  = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.max_uri_handlers  = 4;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t info = {
        .uri = "/info", .method = HTTP_GET,
        .handler = info_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t ota = {
        .uri = "/ota", .method = HTTP_POST,
        .handler = ota_post_handler, .user_ctx = NULL,
    };
    httpd_uri_t logs = {
        .uri = "/logs", .method = HTTP_GET,
        .handler = logs_get_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &logs);

    ESP_LOGI(TAG, "OTA HTTP listening on :80  (POST /ota, GET /info)");
    return ESP_OK;
}
