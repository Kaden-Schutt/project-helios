/*
 * Helios CAMERA DIAGNOSTIC — OTA-enabled
 * ======================================
 *
 * Remote-only iteration loop: push firmware via HTTP OTA, capture
 * frames and logs over HTTP, no USB needed after the initial flash.
 *
 * Boot sequence:
 *   1. Print banner (chip, heap, reset reason, cam pins)
 *   2. Raw SCCB I2C scan (expect false negative without XCLK; esp-camera
 *      init does its own probe with XCLK enabled).
 *   3. Init OV3660 — if it fails here, hardware issue.
 *   4. PHASE A: 30 back-to-back idle captures + timing/validity stats.
 *   5. Bring up WiFi, mDNS (helios-cam.local), OTA HTTP server on :80.
 *   6. Endurance loop: 1 capture per second, keep latest in RAM, print
 *      10s summaries. Stays up forever so OTA is always reachable.
 *
 * Endpoints:
 *   GET  /info      — JSON status (app, uptime, partition, heap)
 *   GET  /logs      — ring-buffered [CAMDIAG] lines (text/plain)
 *   GET  /frame     — latest captured JPEG (image/jpeg)
 *   POST /ota       — firmware.bin body, flashes + reboots
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "driver/i2c_master.h"
#include "mdns.h"

#include "camera_helios.h"
#include "wifi_helios.h"
#include "wifi_credentials.h"
#include "ota.h"
#include "diag_log.h"
#include "button.h"
#include "mic_probe.h"
#include "sd_card.h"
#include "recovery.h"

/* Shared last-frame buffer for /frame endpoint. The endurance loop
 * captures every ~1 s and copies into this PSRAM-backed buffer under
 * s_frame_mutex. The HTTP handler reads it under the same mutex. */
uint8_t *g_last_frame = NULL;
size_t   g_last_frame_len = 0;
uint32_t g_last_frame_id = 0;
int64_t  g_last_frame_ts_us = 0;
SemaphoreHandle_t g_frame_mutex = NULL;

static const char *rr_name(esp_reset_reason_t rr)
{
    switch (rr) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        default:                return "unknown";
    }
}

static void banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    esp_reset_reason_t rr = esp_reset_reason();
    const esp_partition_t *running = esp_ota_get_running_partition();

    DLOG("\n[CAMDIAG] ================================================\n");
    DLOG("[CAMDIAG] Helios Camera Diagnostic (OTA)\n");
    DLOG("[CAMDIAG] ================================================\n");
    DLOG("[CAMDIAG] chip=%s cores=%d rev=%d flash=%u MB\n",
         info.model == CHIP_ESP32S3 ? "ESP32-S3" : "unknown",
         info.cores, info.revision, (unsigned)(flash_size / (1024 * 1024)));
    DLOG("[CAMDIAG] reset_reason=%d (%s)%s\n", rr, rr_name(rr),
         rr == ESP_RST_BROWNOUT ? "  <-- BROWNOUT" : "");
    DLOG("[CAMDIAG] running_partition=%s\n",
         running ? running->label : "?");
    DLOG("[CAMDIAG] heap internal=%u psram=%u largest_psram=%u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    DLOG("[CAMDIAG] cam pins: XCLK=%d SIOD=%d SIOC=%d  PCLK=%d\n",
         CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC, CAM_PIN_PCLK);
}

/* --- Raw SCCB I2C scan (informational; OV sensors without XCLK won't ACK) --- */
static void i2c_scan_sccb(void)
{
    DLOG("[CAMDIAG] ---- PHASE 0: SCCB I2C scan (no XCLK yet) ----\n");
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CAM_PIN_SIOD,
        .scl_io_num = CAM_PIN_SIOC,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t e = i2c_new_master_bus(&cfg, &bus);
    DLOG("[CAMDIAG] I2C init err=0x%x\n", e);
    if (e != ESP_OK) return;
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            DLOG("[CAMDIAG] I2C device @ 0x%02x\n", addr);
            found++;
        }
    }
    DLOG("[CAMDIAG] I2C scan: %d device(s). "
         "(OV sensors don't ACK without XCLK — this is informational.)\n", found);
    i2c_del_master_bus(bus);
}

static int jpeg_valid(const uint8_t *b, size_t n)
{
    return n >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF
        && b[n - 2] == 0xFF && b[n - 1] == 0xD9;
}

/* PHASE A: idle capture batch. Short (30 frames) so we don't delay WiFi. */
static void phase_a(void)
{
    DLOG("[CAMDIAG] ---- PHASE A_IDLE: 30 captures ----\n");
    int ok = 0, fail = 0, invalid = 0;
    int64_t tmin = INT64_MAX, tmax = 0, tsum = 0;
    size_t smin = SIZE_MAX, smax = 0;
    uint64_t ssum = 0;

    for (int i = 0; i < 30; i++) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - t0;
        if (err != ESP_OK || !buf || len == 0) {
            fail++;
            DLOG("[CAMDIAG] A cap#%d FAIL err=0x%x\n", i, err);
            camera_helios_return_fb();
            continue;
        }
        if (!jpeg_valid(buf, len)) invalid++;
        ok++;
        if (dt < tmin) tmin = dt;
        if (dt > tmax) tmax = dt;
        tsum += dt;
        if (len < smin) smin = len;
        if (len > smax) smax = len;
        ssum += len;
        camera_helios_return_fb();
    }
    int64_t avg = ok ? tsum / ok : 0;
    size_t savg = ok ? (size_t)(ssum / ok) : 0;
    DLOG("[CAMDIAG] A_IDLE ok=%d fail=%d invalid=%d\n", ok, fail, invalid);
    if (ok) {
        DLOG("[CAMDIAG] A timing_us min=%lld avg=%lld max=%lld jitter=%lld\n",
             (long long)tmin, (long long)avg, (long long)tmax,
             (long long)(tmax - tmin));
        DLOG("[CAMDIAG] A size_bytes min=%u avg=%u max=%u\n",
             (unsigned)smin, (unsigned)savg, (unsigned)smax);
    }
}

static esp_err_t mdns_up(const char *ip_str)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    DLOG("[CAMDIAG] mDNS: %s.local -> %s\n", MDNS_HOSTNAME, ip_str);
    return ESP_OK;
}

/* Copy the current camera fb into the shared g_last_frame buffer. */
static void publish_frame(uint8_t *buf, size_t len)
{
    if (!g_frame_mutex) return;
    xSemaphoreTake(g_frame_mutex, portMAX_DELAY);
    if (g_last_frame && g_last_frame_len < len) {
        heap_caps_free(g_last_frame);
        g_last_frame = NULL;
    }
    if (!g_last_frame) {
        size_t cap = len < 32 * 1024 ? 32 * 1024 : len;
        g_last_frame = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    }
    if (g_last_frame) {
        memcpy(g_last_frame, buf, len);
        g_last_frame_len = len;
        g_last_frame_id++;
        g_last_frame_ts_us = esp_timer_get_time();
    }
    xSemaphoreGive(g_frame_mutex);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    dlog_init();
    banner();

    g_frame_mutex = xSemaphoreCreateMutex();

    /* ---- Early boot: SD + recovery check BEFORE WiFi ---- */
    sd_card_mount();    /* no-op if no card; is_mounted() returns false */
    recovery_boot_check();  /* may reboot into SD recovery image and not return */

    i2c_scan_sccb();

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = camera_helios_init();
    int64_t init_us = esp_timer_get_time() - t0;
    DLOG("[CAMDIAG] camera_helios_init err=0x%x took_us=%lld ready=%d\n",
         err, (long long)init_us, camera_helios_is_ready());
    if (err != ESP_OK) {
        DLOG("[CAMDIAG] camera init failed — WiFi+OTA still coming up so you "
             "can push a new fw without USB.\n");
    } else {
        phase_a();
    }

    /* ---- WiFi + mDNS + OTA server ---- */
    /* Assemble candidate list: SD override (one or many), else compiled
     * default. Try each in priority order until one connects. */
    wifi_entry_t nets[6];
    int n_nets = sd_card_read_wifi_list(nets, 6);
    if (n_nets == 0) {
        strncpy(nets[0].ssid, WIFI_SSID, sizeof(nets[0].ssid) - 1);
        strncpy(nets[0].psk,  WIFI_PASSWORD, sizeof(nets[0].psk) - 1);
        n_nets = 1;
        DLOG("[CAMDIAG] no wifi.conf — using compiled default \"%s\"\n",
             nets[0].ssid);
    }

    esp_err_t wer = ESP_FAIL;
    for (int i = 0; i < n_nets; i++) {
        DLOG("[CAMDIAG] (%d/%d) trying SSID=\"%s\"\n",
             i + 1, n_nets, nets[i].ssid);
        wer = wifi_helios_init(nets[i].ssid, nets[i].psk, 12000);
        if (wer == ESP_OK) {
            DLOG("[CAMDIAG] connected to \"%s\"\n", nets[i].ssid);
            break;
        }
        DLOG("[CAMDIAG] SSID \"%s\" failed (0x%x); trying next\n",
             nets[i].ssid, wer);
        wifi_helios_deinit();
    }
    if (wer == ESP_OK) {
        char ip[32] = "?";
        wifi_helios_get_ip(ip, sizeof(ip));
        DLOG("[CAMDIAG] WiFi OK, IP=%s\n", ip);
        mdns_up(ip);
        esp_err_t oer = ota_http_start();
        DLOG("[CAMDIAG] ota_http_start err=0x%x\n", oer);
        DLOG("[CAMDIAG] sign: python3 scripts/sign_ota.py firmware.bin\n");
        DLOG("[CAMDIAG] push: curl --data-binary @firmware.signed.bin "
             "http://%s.local/ota\n", MDNS_HOSTNAME);
        DLOG("[CAMDIAG] frame: curl http://%s.local/frame > snap.jpg\n", MDNS_HOSTNAME);
        /* Tier 1+2: app healthy — cancel rollback, promote SD recovery staging. */
        recovery_mark_app_valid();
    } else {
        DLOG("[CAMDIAG] WiFi FAILED (err=0x%x) — NOT marking valid.\n", wer);
        /* Immediate Tier-3 pivot: if a signed BLE rescue image is staged on
         * SD, jump into it right now so the device becomes reachable via
         * Bluetooth instead of hanging waiting for WiFi that isn't there. */
        recovery_pivot_to_ble();
        /* If pivot didn't reboot, fall through — rollback/SD-recovery
         * from Tiers 1/2 will kick in on subsequent boots. */
    }

    /* ---- Peripheral probes: button + mic ---- */
    button_init();
    mic_probe_init();

    /* ---- Endurance loop: 1 fps, publish frames, 10s summary ---- */
    if (!camera_helios_is_ready()) {
        DLOG("[CAMDIAG] Camera not ready, idling forever (OTA still works).\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(60000));
    }

    DLOG("[CAMDIAG] ---- PHASE D: endurance @ 1 fps, 10s summaries ----\n");
    int ok = 0, fail = 0;
    int64_t min_dt = INT64_MAX, max_dt = 0, sum_dt = 0, n_timed = 0;
    int64_t last_report = esp_timer_get_time();
    int64_t boot = last_report;

    while (1) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int64_t tc0 = esp_timer_get_time();
        esp_err_t e = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - tc0;
        if (e == ESP_OK && buf && len) {
            ok++;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            sum_dt += dt;
            n_timed++;
            publish_frame(buf, len);
            camera_helios_return_fb();
        } else {
            fail++;
            DLOG("[CAMDIAG] D cap FAIL err=0x%x\n", e);
            camera_helios_return_fb();
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report >= 10LL * 1000 * 1000) {
            int64_t avg = n_timed ? sum_dt / n_timed : 0;
            DLOG("[CAMDIAG] D uptime=%llds ok=%d fail=%d "
                 "min=%lldus avg=%lldus max=%lldus heap=%u psram=%u\n",
                 (long long)((now - boot) / 1000000), ok, fail,
                 (long long)(n_timed ? min_dt : 0),
                 (long long)avg, (long long)max_dt,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_report = now;
            min_dt = INT64_MAX; max_dt = 0; sum_dt = 0; n_timed = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
