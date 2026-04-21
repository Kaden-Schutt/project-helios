/*
 * Goouuu ESP32-S3-CAM — Camera Diagnostic
 * =======================================
 * Same diag structure as the XIAO Sense version, adapted for the Goouuu
 * S3-CAM board (different DVP/SCCB pin map, no onboard mic).
 *
 *   PHASE 0: raw I2C scan on SDA=4/SCL=5 (proves sensor is on the bus)
 *   PHASE A: 100 back-to-back captures, latency + JPEG validity stats
 *   PHASE D: endurance loop @ 2 fps until you stop listening
 *
 * Verdict logic:
 *   PHASE 0 finds device @ 0x30/0x3C/etc → sensor alive. If init
 *   succeeds and PHASE A captures JPEGs → XIAO Sense board had the
 *   problem, camera module is fine.
 *   PHASE 0 finds nothing → sensor module is dead.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "camera_helios.h"

static const char *rr_str(esp_reset_reason_t rr)
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

    printf("\n[CAMDIAG] ==============================================\n");
    printf("[CAMDIAG] Goouuu ESP32-S3-CAM Camera Diagnostic\n");
    printf("[CAMDIAG] ==============================================\n");
    printf("[CAMDIAG] chip=%s cores=%d rev=%d flash=%u MB\n",
           info.model == CHIP_ESP32S3 ? "ESP32-S3" : "unknown",
           info.cores, info.revision, (unsigned)(flash_size / (1024 * 1024)));
    printf("[CAMDIAG] reset_reason=%d (%s)%s\n", rr, rr_str(rr),
           rr == ESP_RST_BROWNOUT ? "  <-- BROWNOUT" : "");
    printf("[CAMDIAG] heap internal=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("[CAMDIAG] cam pins: XCLK=%d SIOD=%d SIOC=%d  "
           "D0..D7=%d,%d,%d,%d,%d,%d,%d,%d  VSYNC=%d HREF=%d PCLK=%d\n",
           CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC,
           CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
           CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
           CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK);
}

static void i2c_scan(void)
{
    printf("[CAMDIAG] ---- PHASE 0: SCCB I2C scan on SDA=%d SCL=%d ----\n",
           CAM_PIN_SIOD, CAM_PIN_SIOC);

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
    printf("[CAMDIAG] I2C init err=0x%x\n", e);
    if (e != ESP_OK) return;

    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            printf("[CAMDIAG] I2C device @ 0x%02x\n", addr);
            found++;
        }
    }
    if (!found) {
        printf("[CAMDIAG] I2C RESULT: NO devices on bus — "
               "camera ribbon not seated, damaged, or sensor is dead.\n");
    } else {
        printf("[CAMDIAG] I2C RESULT: %d device(s) found (sensor is alive)\n", found);
    }
    i2c_del_master_bus(bus);
}

static int jpeg_valid(const uint8_t *b, size_t n)
{
    if (n < 4) return 0;
    if (b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return 0;
    if (b[n - 2] != 0xFF || b[n - 1] != 0xD9) return 0;
    return 1;
}

static void capture_batch(const char *name, int n)
{
    printf("[CAMDIAG] ---- %s: %d captures ----\n", name, n);
    int ok = 0, fail = 0, invalid = 0;
    int64_t tmin = INT64_MAX, tmax = 0, tsum = 0;
    size_t smin = SIZE_MAX, smax = 0;
    uint64_t ssum = 0;

    for (int i = 0; i < n; i++) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - t0;
        if (err != ESP_OK || !buf || len == 0) {
            fail++;
            printf("[CAMDIAG] %s cap#%d FAIL err=0x%x dt=%lldus\n",
                   name, i, err, (long long)dt);
            camera_helios_return_fb();
            continue;
        }
        int valid = jpeg_valid(buf, len);
        if (!valid) invalid++;
        ok++;
        if (dt < tmin) tmin = dt;
        if (dt > tmax) tmax = dt;
        tsum += dt;
        if (len < smin) smin = len;
        if (len > smax) smax = len;
        ssum += len;
        if (i < 3 || i == n - 1 || i % 25 == 0) {
            printf("[CAMDIAG] %s cap#%d dt=%lldus size=%u valid=%d "
                   "soi=%02x%02x eoi=%02x%02x\n",
                   name, i, (long long)dt, (unsigned)len, valid,
                   buf[0], buf[1], buf[len - 2], buf[len - 1]);
        }
        camera_helios_return_fb();
    }

    int64_t avg = ok ? tsum / ok : 0;
    size_t savg = ok ? (size_t)(ssum / ok) : 0;
    printf("[CAMDIAG] %s SUMMARY ok=%d fail=%d invalid=%d\n",
           name, ok, fail, invalid);
    if (ok) {
        printf("[CAMDIAG] %s timing_us min=%lld avg=%lld max=%lld jitter=%lld\n",
               name, (long long)tmin, (long long)avg, (long long)tmax,
               (long long)(tmax - tmin));
        printf("[CAMDIAG] %s size_bytes min=%u avg=%u max=%u\n",
               name, (unsigned)smin, (unsigned)savg, (unsigned)smax);
    }
    printf("[CAMDIAG] %s heap_after internal=%u psram=%u\n",
           name,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    banner();
    i2c_scan();

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = camera_helios_init();
    int64_t init_us = esp_timer_get_time() - t0;
    printf("[CAMDIAG] camera_helios_init err=0x%x took_us=%lld ready=%d\n",
           err, (long long)init_us, camera_helios_is_ready());

    if (err != ESP_OK) {
        printf("[CAMDIAG] camera init failed — halting. Check I2C scan "
               "above: if no devices found, sensor is dead; if a device "
               "was found, CONFIG_*_SUPPORT is wrong for this sensor.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(60000));
    }

    capture_batch("A_IDLE", 100);

    printf("[CAMDIAG] ---- PHASE D: endurance @ 2 fps, 10s summary ----\n");
    int tot_ok = 0, tot_fail = 0, consec_fail = 0;
    int64_t min_dt = INT64_MAX, max_dt = 0, sum_dt = 0, n_timed = 0;
    int64_t last_report = esp_timer_get_time();
    int64_t boot = esp_timer_get_time();

    while (1) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int64_t tc0 = esp_timer_get_time();
        esp_err_t e = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - tc0;
        if (e == ESP_OK && buf && len) {
            tot_ok++; consec_fail = 0;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            sum_dt += dt; n_timed++;
            camera_helios_return_fb();
        } else {
            tot_fail++; consec_fail++;
            printf("[CAMDIAG] D cap FAIL err=0x%x consec=%d\n", e, consec_fail);
            camera_helios_return_fb();
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report >= 10LL * 1000 * 1000) {
            int64_t avg = n_timed ? sum_dt / n_timed : 0;
            printf("[CAMDIAG] D uptime=%llds ok=%d fail=%d "
                   "min=%lldus avg=%lldus max=%lldus heap=%u psram=%u\n",
                   (long long)((now - boot) / 1000000),
                   tot_ok, tot_fail,
                   (long long)(n_timed ? min_dt : 0), (long long)avg,
                   (long long)max_dt,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_report = now;
            min_dt = INT64_MAX; max_dt = 0; sum_dt = 0; n_timed = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
