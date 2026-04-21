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
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "diag_log.h"
#include "button.h"
#include "mic_probe.h"
#include "ota_verify.h"
#include "ota_pubkey.h"
#include "sd_card.h"

static const char *TAG = "ota";
static httpd_handle_t s_server = NULL;

/* Defined in main.c — endurance loop publishes latest JPEG into this
 * shared buffer under g_frame_mutex. */
extern uint8_t *g_last_frame;
extern size_t   g_last_frame_len;
extern uint32_t g_last_frame_id;
extern int64_t  g_last_frame_ts_us;
extern SemaphoreHandle_t g_frame_mutex;

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    /* Stage in PSRAM so we don't consume internal heap for 16 KB. */
    static char *buf = NULL;
    if (!buf) buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf)  return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    int n = dlog_dump(buf, 16384);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const size_t CHUNK = 4096;
    int off = 0;
    esp_err_t e = ESP_OK;
    while (off < n) {
        size_t c = n - off > (int)CHUNK ? CHUNK : (size_t)(n - off);
        e = httpd_resp_send_chunk(req, buf + off, c);
        if (e != ESP_OK) break;
        off += c;
    }
    if (e == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

static esp_err_t frame_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "/frame request");
    if (!g_frame_mutex) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no buffer");
    }
    xSemaphoreTake(g_frame_mutex, portMAX_DELAY);
    if (!g_last_frame || g_last_frame_len == 0) {
        xSemaphoreGive(g_frame_mutex);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame yet");
    }
    size_t len = g_last_frame_len;
    uint32_t id = g_last_frame_id;
    int64_t ts = g_last_frame_ts_us;

    char idbuf[32], agebuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)id);
    snprintf(agebuf, sizeof(agebuf), "%lld",
             (long long)((esp_timer_get_time() - ts) / 1000));
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "X-Frame-Id", idbuf);
    httpd_resp_set_hdr(req, "X-Frame-Age-Ms", agebuf);

    /* Send in chunks so a slow network can't starve the httpd worker,
     * and so we don't block the endurance loop's publisher indefinitely.
     * 4 KB chunks is the standard LWIP sweet spot. */
    const size_t CHUNK = 4096;
    size_t off = 0;
    esp_err_t e = ESP_OK;
    while (off < len) {
        size_t n = len - off > CHUNK ? CHUNK : len - off;
        e = httpd_resp_send_chunk(req, (const char *)(g_last_frame + off), n);
        if (e != ESP_OK) break;
        off += n;
    }
    if (e == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);  /* end marker */
    xSemaphoreGive(g_frame_mutex);
    ESP_LOGI(TAG, "/frame sent id=%lu len=%u err=0x%x",
             (unsigned long)id, (unsigned)len, e);
    return e;
}

static esp_err_t button_get_handler(httpd_req_t *req)
{
    char body[192];
    uint32_t age = button_last_press_ms_ago();
    char age_buf[24];
    if (age == UINT32_MAX) snprintf(age_buf, sizeof(age_buf), "null");
    else                    snprintf(age_buf, sizeof(age_buf), "%lu", (unsigned long)age);
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"pressed\": %s,\n"
        "  \"presses_total\": %lu,\n"
        "  \"last_press_ms_ago\": %s\n"
        "}\n",
        button_is_pressed() ? "true" : "false",
        (unsigned long)button_press_count(),
        age_buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* 4-second scan of candidate pins: 2 s with pulldown, 2 s with pullup.
 * Reports min/max per pin for each mode. A button's presence shows up as
 * min=0/max=1 (or 1/0) in one of the modes. Pulldown mode catches
 * active-high buttons; pullup mode catches open-drain/active-low. */
#include "driver/gpio.h"
static esp_err_t pins_get_handler(httpd_req_t *req)
{
    static const int pins[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 21, 43, 44 };
    const int n_pins = sizeof(pins) / sizeof(pins[0]);
    int mn_pd[16], mx_pd[16], mn_pu[16], mx_pu[16];

    for (int mode = 0; mode < 2; mode++) {
        bool pu = (mode == 1);
        for (int i = 0; i < n_pins; i++) {
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << pins[i],
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = pu ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                .pull_down_en = pu ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&cfg);
        }
        vTaskDelay(pdMS_TO_TICKS(20));  /* let pulls settle */
        int mn[16], mx[16];
        for (int i = 0; i < n_pins; i++) { mn[i] = 1; mx[i] = 0; }
        for (int s = 0; s < 200; s++) {
            for (int i = 0; i < n_pins; i++) {
                int v = gpio_get_level(pins[i]);
                if (v < mn[i]) mn[i] = v;
                if (v > mx[i]) mx[i] = v;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        for (int i = 0; i < n_pins; i++) {
            if (pu) { mn_pu[i] = mn[i]; mx_pu[i] = mx[i]; }
            else    { mn_pd[i] = mn[i]; mx_pd[i] = mx[i]; }
        }
    }

    char body[1200];
    int off = snprintf(body, sizeof(body), "{\n");
    for (int i = 0; i < n_pins; i++) {
        off += snprintf(body + off, sizeof(body) - off,
            "  \"gpio%d\": {\"pd\": [%d,%d], \"pu\": [%d,%d]}%s\n",
            pins[i], mn_pd[i], mx_pd[i], mn_pu[i], mx_pu[i],
            i == n_pins - 1 ? "" : ",");
    }
    snprintf(body + off, sizeof(body) - off, "}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t wifi_conf_get_handler(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD");
    }
    FILE *f = sd_card_open_read("/sd/wifi.conf");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no wifi.conf");
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t wifi_conf_post_handler(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD");
    }
    if (req->content_len <= 0 || req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty or too big");
    }
    FILE *f = sd_card_open_write("/sd/wifi.conf.tmp");
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");

    char buf[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got <= 0) { fclose(f); return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv"); }
        if ((int)fwrite(buf, 1, got, f) != got) {
            fclose(f);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        remaining -= got;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!sd_card_rename("/sd/wifi.conf.tmp", "/sd/wifi.conf")) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    /* FatFS commits directory entries on close/rename, but give it a
     * beat before we potentially reboot. */
    vTaskDelay(pdMS_TO_TICKS(300));
    const char *ok = "OK — /sd/wifi.conf updated. Reboot to apply (POST /reboot or cycle power).\n";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ok, strlen(ok));
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    const char *ok = "rebooting in 2s (unmounting SD first)\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ok, strlen(ok));
    /* Unmount SD first — forces FatFS flush + cleanly releases the card
     * so pending writes (e.g. wifi.conf) actually land on disk. */
    sd_card_unmount();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t mic_get_handler(httpd_req_t *req)
{
    char body[192];
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"rms\": %lu,\n"
        "  \"peak\": %lu,\n"
        "  \"frames_read\": %llu\n"
        "}\n",
        (unsigned long)mic_probe_rms(),
        (unsigned long)mic_probe_peak(),
        (unsigned long long)mic_probe_frames_read());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    uint32_t hi  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t hp  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t hil = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t hpl = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"app_name\": \"%s\",\n"
        "  \"app_version\": \"%s\",\n"
        "  \"compile_time\": \"%s %s\",\n"
        "  \"idf_version\": \"%s\",\n"
        "  \"running_partition\": \"%s\",\n"
        "  \"boot_partition\": \"%s\",\n"
        "  \"heap_internal_free\": %lu,\n"
        "  \"heap_internal_largest\": %lu,\n"
        "  \"heap_psram_free\": %lu,\n"
        "  \"heap_psram_largest\": %lu,\n"
        "  \"uptime_s\": %lld\n"
        "}\n",
        app->project_name, app->version, app->date, app->time,
        app->idf_ver,
        running ? running->label : "?",
        boot ? boot->label : "?",
        (unsigned long)hi, (unsigned long)hil,
        (unsigned long)hp, (unsigned long)hpl,
        (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA POST: content-length=%d", req->content_len);
    printf("[OTA] incoming upload, %d bytes expected (firmware + 32-byte HMAC sig)\n",
           req->content_len);

    if (req->content_len < (int)(OTA_SIG_LEN + 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too small");
        return ESP_FAIL;
    }
    int firmware_len = req->content_len - OTA_SIG_LEN;

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
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

    /* Also stage to SD (if mounted) so on mark_valid we have a recovery blob. */
    FILE *sd_staging = sd_card_open_write("/sd/recovery.staging.bin");

    ota_verify_ctx_t *vctx = NULL;
    if (ota_verify_start(&vctx) != ESP_OK) {
        esp_ota_abort(handle);
        if (sd_staging) fclose(sd_staging);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "hmac init");
        return ESP_FAIL;
    }

    char buf[2048];
    uint8_t sig_buf[OTA_SIG_LEN];
    int sig_pos = 0;
    int bytes_seen = 0;
    int64_t t0 = esp_timer_get_time();
    while (bytes_seen < req->content_len) {
        int remaining = req->content_len - bytes_seen;
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            esp_ota_abort(handle);
            ota_verify_abort(vctx);
            if (sd_staging) fclose(sd_staging);
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv failed");
            return ESP_FAIL;
        }

        /* Everything in this chunk that belongs to the firmware body goes
         * into HMAC + OTA partition. Anything past firmware_len is signature. */
        int fw_in_chunk = 0, sig_in_chunk = 0;
        if (bytes_seen + got <= firmware_len) {
            fw_in_chunk = got;
        } else if (bytes_seen >= firmware_len) {
            sig_in_chunk = got;
        } else {
            fw_in_chunk = firmware_len - bytes_seen;
            sig_in_chunk = got - fw_in_chunk;
        }

        if (fw_in_chunk > 0) {
            ota_verify_update(vctx, (uint8_t *)buf, fw_in_chunk);
            err = esp_ota_write(handle, buf, fw_in_chunk);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
                esp_ota_abort(handle);
                ota_verify_abort(vctx);
                if (sd_staging) fclose(sd_staging);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    esp_err_to_name(err));
                return ESP_FAIL;
            }
        }
        if (sig_in_chunk > 0) {
            int copy = sig_in_chunk;
            if (sig_pos + copy > OTA_SIG_LEN) copy = OTA_SIG_LEN - sig_pos;
            memcpy(sig_buf + sig_pos, buf + fw_in_chunk, copy);
            sig_pos += copy;
        }
        if (sd_staging) fwrite(buf, 1, got, sd_staging);
        bytes_seen += got;
        if ((bytes_seen & 0xFFFF) == 0) {
            ESP_LOGI(TAG, "progress %d / %d", bytes_seen, req->content_len);
        }
    }
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "received %d bytes in %lld ms", bytes_seen, dt / 1000);
    if (sd_staging) { fflush(sd_staging); fclose(sd_staging); }

    err = ota_verify_finish(vctx, sig_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SIGNATURE MISMATCH — rejecting upload");
        esp_ota_abort(handle);
        sd_card_unlink("/sd/recovery.staging.bin");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad signature");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "signature OK, finalizing OTA");

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
    cfg.stack_size  = 12288;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.max_uri_handlers  = 16;
    cfg.lru_purge_enable  = true;

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
    httpd_uri_t frame = {
        .uri = "/frame", .method = HTTP_GET,
        .handler = frame_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t button = {
        .uri = "/button", .method = HTTP_GET,
        .handler = button_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t mic = {
        .uri = "/mic", .method = HTTP_GET,
        .handler = mic_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t pins = {
        .uri = "/pins", .method = HTTP_GET,
        .handler = pins_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t wifi_get = {
        .uri = "/wifi", .method = HTTP_GET,
        .handler = wifi_conf_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t wifi_post = {
        .uri = "/wifi", .method = HTTP_POST,
        .handler = wifi_conf_post_handler, .user_ctx = NULL,
    };
    httpd_uri_t reboot = {
        .uri = "/reboot", .method = HTTP_POST,
        .handler = reboot_post_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &logs);
    httpd_register_uri_handler(s_server, &frame);
    httpd_register_uri_handler(s_server, &button);
    httpd_register_uri_handler(s_server, &mic);
    httpd_register_uri_handler(s_server, &pins);
    httpd_register_uri_handler(s_server, &wifi_get);
    httpd_register_uri_handler(s_server, &wifi_post);
    httpd_register_uri_handler(s_server, &reboot);

    ESP_LOGI(TAG, "OTA HTTP listening on :80  "
             "(GET /info /logs /frame /button /mic, POST /ota)");
    return ESP_OK;
}
