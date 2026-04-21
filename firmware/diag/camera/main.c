/*
 * Helios CAMERA DIAGNOSTIC FIRMWARE
 * =================================
 * Standalone fw that:
 *   1. Prints chip + heap + reset-reason banner over USB serial
 *   2. Inits OV3660 via esp-camera driver
 *   3. Phase A: 100 idle back-to-back captures, measures min/avg/max
 *              acquisition latency, jitter, JPEG validity, JPEG size.
 *   4. Phase B: starts a PDM-mic consumer task (reproduces FB-OVF
 *              condition from real workload), repeats 100 captures.
 *   5. Phase C: if B has failures, deinit+reinit the camera driver
 *              and run 30 more captures to measure recovery.
 *   6. Phase D: endurance loop @ ~2 fps forever; per-10s summary
 *              line; auto-reinit after 5 consecutive failures so we
 *              can time how long the camera survives.
 *
 * All lines prefixed "[CAMDIAG]" for easy grep. No WiFi, no speaker.
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
#include "mic_helios.h"

/* Scan the SCCB I2C bus (SDA=40, SCL=39) for any responsive devices.
 * OV2640 answers at 0x30 (7-bit), OV3660/OV5640 at 0x3C, various vendor
 * sensors at 0x21, 0x24, etc. Scanning all 0x03..0x77 is the definitive
 * check for "is there a camera on the bus at all". */
static void i2c_scan_sccb(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 40,
        .scl_io_num = 39,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t e = i2c_new_master_bus(&cfg, &bus);
    printf("[CAMDIAG] I2C_SCAN init err=0x%x (SDA=40 SCL=39, pullup=on)\n", e);
    if (e != ESP_OK) return;

    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        esp_err_t probe = i2c_master_probe(bus, addr, 50);
        if (probe == ESP_OK) {
            printf("[CAMDIAG] I2C_SCAN  device responded @ 0x%02x\n", addr);
            found++;
        }
    }
    if (!found) {
        printf("[CAMDIAG] I2C_SCAN RESULT: NO devices on SCCB bus "
               "— camera ribbon is not connected, ribbon is damaged, "
               "or sensor has failed. Check physical connection.\n");
    } else {
        printf("[CAMDIAG] I2C_SCAN RESULT: %d device(s) found\n", found);
    }
    i2c_del_master_bus(bus);
}

static const char *TAG_RESET(esp_reset_reason_t rr)
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
        case ESP_RST_EXT:       return "ext";
        default:                return "unknown";
    }
}

static void print_banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    esp_reset_reason_t rr = esp_reset_reason();

    printf("\n[CAMDIAG] ================================================\n");
    printf("[CAMDIAG] Helios Camera Diagnostic\n");
    printf("[CAMDIAG] ================================================\n");
    printf("[CAMDIAG] chip=%s cores=%d rev=%d flash=%u MB\n",
           info.model == CHIP_ESP32S3 ? "ESP32-S3" : "unknown",
           info.cores, info.revision, (unsigned)(flash_size / (1024 * 1024)));
    printf("[CAMDIAG] reset_reason=%d (%s)%s\n", rr, TAG_RESET(rr),
           rr == ESP_RST_BROWNOUT ? "  <-- BROWNOUT DETECTED" : "");
    printf("[CAMDIAG] heap_default=%u internal=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("[CAMDIAG] heap_largest_internal=%u largest_psram=%u\n",
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    printf("[CAMDIAG] cam pins: XCLK=10 SIOD=40 SIOC=39 "
           "D0..D7=15,17,18,16,14,12,11,48 VSYNC=38 HREF=47 PCLK=13\n");
}

static int jpeg_valid(const uint8_t *b, size_t n)
{
    if (n < 4) return 0;
    if (b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return 0;
    if (b[n - 2] != 0xFF || b[n - 1] != 0xD9) return 0;
    return 1;
}

typedef struct {
    int attempts;
    int ok;
    int failed;
    int invalid_jpeg;
    int64_t t_min_us;
    int64_t t_max_us;
    int64_t t_sum_us;
    size_t size_min;
    size_t size_max;
    uint64_t size_sum;
} stats_t;

static void stats_reset(stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->t_min_us = INT64_MAX;
    s->size_min = SIZE_MAX;
}

static void stats_print(const char *name, const stats_t *s)
{
    int64_t avg = s->ok ? s->t_sum_us / s->ok : 0;
    size_t savg = s->ok ? (size_t)(s->size_sum / s->ok) : 0;
    int64_t tmin = s->ok ? s->t_min_us : 0;
    size_t smin = s->ok ? s->size_min : 0;
    printf("[CAMDIAG] %s SUMMARY: attempts=%d ok=%d failed=%d invalid_jpeg=%d\n",
           name, s->attempts, s->ok, s->failed, s->invalid_jpeg);
    printf("[CAMDIAG] %s timing_us: min=%lld avg=%lld max=%lld jitter=%lld\n",
           name, (long long)tmin, (long long)avg, (long long)s->t_max_us,
           (long long)(s->ok ? (s->t_max_us - tmin) : 0));
    printf("[CAMDIAG] %s size_bytes: min=%u avg=%u max=%u\n",
           name, (unsigned)smin, (unsigned)savg, (unsigned)s->size_max);
}

static int run_capture_batch(const char *phase, int n, stats_t *s)
{
    printf("[CAMDIAG] ---- PHASE %s: %d captures ----\n", phase, n);
    stats_reset(s);
    for (int i = 0; i < n; i++) {
        s->attempts++;
        int64_t t0 = esp_timer_get_time();
        uint8_t *buf = NULL;
        size_t len = 0;
        esp_err_t err = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - t0;
        if (err != ESP_OK || !buf || len == 0) {
            s->failed++;
            printf("[CAMDIAG] %s cap#%d FAIL err=0x%x dt=%lldus\n",
                   phase, i, err, (long long)dt);
            camera_helios_return_fb();
            continue;
        }
        int valid = jpeg_valid(buf, len);
        if (!valid) s->invalid_jpeg++;
        s->ok++;
        if (dt < s->t_min_us) s->t_min_us = dt;
        if (dt > s->t_max_us) s->t_max_us = dt;
        s->t_sum_us += dt;
        if (len < s->size_min) s->size_min = len;
        if (len > s->size_max) s->size_max = len;
        s->size_sum += len;
        if (i < 3 || i == n - 1 || i % 20 == 0) {
            printf("[CAMDIAG] %s cap#%d dt=%lldus size=%u valid=%d soi=%02x%02x eoi=%02x%02x\n",
                   phase, i, (long long)dt, (unsigned)len,
                   valid, buf[0], buf[1], buf[len - 2], buf[len - 1]);
        }
        camera_helios_return_fb();
    }
    stats_print(phase, s);
    printf("[CAMDIAG] %s heap_after: internal=%u psram=%u\n",
           phase,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return s->ok;
}

/* Continuously drain the mic DMA so the PDM channel is always busy.
 * This reproduces the condition under which FB-OVF occurred in the
 * production firmware (mic recording concurrent with camera). */
static void mic_stress_task(void *arg)
{
    volatile bool *run = (volatile bool *)arg;
    uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL);
    if (!buf) { vTaskDelete(NULL); return; }
    uint64_t total_bytes = 0;
    int64_t t0 = esp_timer_get_time();
    while (*run) {
        size_t got = 0;
        mic_helios_read(buf, 8192, &got, 50);
        total_bytes += got;
    }
    int64_t dt = esp_timer_get_time() - t0;
    unsigned kbps = dt ? (unsigned)((total_bytes * 8000ULL) / (uint64_t)(dt / 1000)) : 0;
    printf("[CAMDIAG] mic_stress_task stopped: %llu bytes in %lld us (~%u kbps)\n",
           (unsigned long long)total_bytes, (long long)dt, kbps);
    heap_caps_free(buf);
    vTaskDelete(NULL);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    print_banner();

    /* ----- PHASE 0: raw SCCB bus scan ----- */
    printf("[CAMDIAG] ---- PHASE 0: SCCB I2C BUS SCAN ----\n");
    i2c_scan_sccb();

    /* ----- Init ----- */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = camera_helios_init();
    int64_t init_us = esp_timer_get_time() - t0;
    printf("[CAMDIAG] camera_init: err=0x%x took_us=%lld ready=%d\n",
           err, (long long)init_us, camera_helios_is_ready());
    if (err != ESP_OK) {
        printf("[CAMDIAG] FATAL: init failed. Retrying every 5s (max 10).\n");
        for (int r = 0; r < 10 && err != ESP_OK; r++) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            err = camera_helios_init();
            printf("[CAMDIAG] retry %d: err=0x%x ready=%d\n",
                   r, err, camera_helios_is_ready());
        }
        if (err != ESP_OK) {
            printf("[CAMDIAG] camera dead — halting diagnostic.\n");
            while (1) vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }

    /* ----- PHASE A: idle captures ----- */
    stats_t sa;
    run_capture_batch("A_IDLE", 100, &sa);

    /* ----- PHASE B: mic-stressed captures ----- */
    printf("[CAMDIAG] Starting mic stressor for PHASE B\n");
    esp_err_t mic_err = mic_helios_init();
    printf("[CAMDIAG] mic_helios_init err=0x%x\n", mic_err);
    volatile bool mic_run = true;
    if (mic_err == ESP_OK) {
        mic_helios_start();
        xTaskCreatePinnedToCore(mic_stress_task, "micstress", 4096,
                                (void *)&mic_run, 5, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
    } else {
        printf("[CAMDIAG] PHASE B running WITHOUT mic stressor (init failed)\n");
    }
    stats_t sb;
    run_capture_batch("B_MICSTRESS", 100, &sb);
    mic_run = false;
    vTaskDelay(pdMS_TO_TICKS(400));
    if (mic_err == ESP_OK) mic_helios_deinit();

    /* ----- PHASE C: recovery (if B failed anything) ----- */
    if (sb.failed > 0 || sb.invalid_jpeg > 0 || !camera_helios_is_ready()) {
        printf("[CAMDIAG] ---- PHASE C: attempting deinit+reinit recovery ----\n");
        camera_helios_deinit();
        vTaskDelay(pdMS_TO_TICKS(300));
        int64_t rt0 = esp_timer_get_time();
        esp_err_t e = camera_helios_init();
        printf("[CAMDIAG] reinit err=0x%x took_us=%lld ready=%d\n",
               e, (long long)(esp_timer_get_time() - rt0),
               camera_helios_is_ready());
        if (e == ESP_OK) {
            stats_t sc;
            run_capture_batch("C_RECOVERY", 30, &sc);
        }
    } else {
        printf("[CAMDIAG] ---- PHASE C: skipped (B had no failures) ----\n");
    }

    /* ----- PHASE D: endurance ----- */
    printf("[CAMDIAG] ---- PHASE D: endurance loop @ 2fps, 10s summaries ----\n");
    int tot_ok = 0, tot_fail = 0, consec_fail = 0, reinit_count = 0;
    int64_t min_dt = INT64_MAX, max_dt = 0, sum_dt = 0, n_timed = 0;
    int64_t last_report = esp_timer_get_time();
    int64_t boot_at = esp_timer_get_time();

    while (1) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int64_t tc0 = esp_timer_get_time();
        esp_err_t e = camera_helios_capture(&buf, &len);
        int64_t dt = esp_timer_get_time() - tc0;
        if (e == ESP_OK && buf && len) {
            tot_ok++;
            consec_fail = 0;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            sum_dt += dt;
            n_timed++;
            camera_helios_return_fb();
        } else {
            tot_fail++;
            consec_fail++;
            printf("[CAMDIAG] D cap FAIL err=0x%x consec=%d t=%llds\n",
                   e, consec_fail,
                   (long long)((esp_timer_get_time() - boot_at) / 1000000));
            camera_helios_return_fb();
            if (consec_fail == 5) {
                printf("[CAMDIAG] D 5 consecutive fails — attempting reinit #%d\n",
                       ++reinit_count);
                camera_helios_deinit();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_err_t e2 = camera_helios_init();
                printf("[CAMDIAG] D reinit err=0x%x ready=%d\n",
                       e2, camera_helios_is_ready());
                consec_fail = 0;
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report >= 10LL * 1000 * 1000) {
            int64_t avg = n_timed ? sum_dt / n_timed : 0;
            int64_t tmin = n_timed ? min_dt : 0;
            printf("[CAMDIAG] D progress: uptime=%llds ok=%d fail=%d reinit=%d "
                   "this10s_min=%lld avg=%lld max=%lld heap=%u psram=%u\n",
                   (long long)((now - boot_at) / 1000000),
                   tot_ok, tot_fail, reinit_count,
                   (long long)tmin, (long long)avg, (long long)max_dt,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_report = now;
            min_dt = INT64_MAX;
            max_dt = 0;
            sum_dt = 0;
            n_timed = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
