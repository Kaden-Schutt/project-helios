/*
 * Autonomous B2B + Peripheral Test
 * Runs entirely on-board, saves all results to SD card.
 * No serial monitoring needed — check /sdcard/results.txt when done.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"

#include "helios.h"

static FILE *logf = NULL;

static void LOG(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (logf) {
        vfprintf(logf, fmt, args);
        fflush(logf);
    }
    // Also print to serial for debug
    va_start(args, fmt);
    vprintf(fmt, args);
    fflush(stdout);
    va_end(args);
}

// ── SD Card Init ──
static sdmmc_card_t *s_card = NULL;

static int init_sd(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) return -1;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_PIN;
    slot.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &s_card) == ESP_OK ? 0 : -1;
}

// ── Mic Init ──
static i2s_chan_handle_t rx_chan = NULL;

static int init_mic(void)
{
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.dma_desc_num = 8;
    ch.dma_frame_num = 1024;
    if (i2s_new_channel(&ch, NULL, &rx_chan) != ESP_OK) return -1;

    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_PIN,
            .din = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    if (i2s_channel_init_pdm_rx_mode(rx_chan, &pdm) != ESP_OK) return -1;
    if (i2s_channel_enable(rx_chan) != ESP_OK) return -1;
    return 0;
}

static int record_to_sd(const char *path, int duration_ms)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t max_bytes = (MIC_SAMPLE_RATE * 2 * duration_ms) / 1000;
    size_t total = 0;
    uint8_t buf[1024];
    int32_t peak = 0;

    while (total < max_bytes) {
        size_t got = 0;
        if (i2s_channel_read(rx_chan, buf, sizeof(buf), &got, pdMS_TO_TICKS(1000)) != ESP_OK) {
            continue;  // retry on timeout
        }
        fwrite(buf, 1, got, f);

        // Track peak level
        int16_t *samples = (int16_t *)buf;
        for (size_t i = 0; i < got / 2; i++) {
            int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) peak = v;
        }
        total += got;
    }

    fclose(f);
    return (int)peak;
}

// ── Camera Init ──
static int init_camera(void)
{
    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10,
        .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14,
        .pin_d3 = 16, .pin_d2 = 18, .pin_d1 = 17, .pin_d0 = 15,
        .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg) == ESP_OK ? 0 : -1;
}

void app_main(void)
{
    printf("DIAG: Starting autonomous test...\n");

    // ── 1. Init SD Card ──
    int sd_ok = init_sd();
    printf("DIAG: SD init %s\n", sd_ok == 0 ? "OK" : "FAIL");
    if (sd_ok != 0) {
        printf("DIAG: Cannot run tests without SD card. Halting.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    logf = fopen("/sdcard/results.txt", "w");
    LOG("═══════════════════════════════════\n");
    LOG("  HELIOS AUTONOMOUS DIAGNOSTIC\n");
    LOG("═══════════════════════════════════\n\n");

    // ── 2. Init Mic ──
    int mic_ok = init_mic();
    LOG("MIC INIT: %s\n", mic_ok == 0 ? "PASS" : "FAIL");

    // ── 3. Init Camera ──
    int cam_ok = init_camera();
    LOG("CAM INIT: %s\n", cam_ok == 0 ? "PASS" : "FAIL");

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        LOG("CAM SENSOR: OV%04x at 0x%02x\n", s->id.PID, s->slv_addr);
        if (s->id.PID == 0x3660) {
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    // ── 4. Pin Activity Test (all B2B pins) ──
    LOG("\n── PIN ACTIVITY (200ms each, camera running) ──\n");

    typedef struct { int gpio; const char *name; } pin_t;
    pin_t pins[] = {
        {15,"CAM_D0"}, {17,"CAM_D1"}, {18,"CAM_D2"}, {16,"CAM_D3"},
        {14,"CAM_D4"}, {12,"CAM_D5"}, {11,"CAM_D6"}, {48,"CAM_D7"},
        {13,"PCLK"}, {38,"VSYNC"}, {47,"HREF"}, {10,"XCLK"},
        {40,"CAM_SDA"}, {39,"CAM_SCL"},
        {42,"MIC_CLK"}, {41,"MIC_DAT"},
        {21,"SD_CS"}, {7,"SD_SCK"}, {8,"SD_MISO"}, {9,"SD_MOSI"},
    };
    int n_pins = sizeof(pins) / sizeof(pins[0]);

    for (int p = 0; p < n_pins; p++) {
        int gpio = pins[p].gpio;
        int transitions = 0;
        int last = gpio_get_level(gpio);
        uint32_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)) {
            int level = gpio_get_level(gpio);
            if (level != last) { transitions++; last = level; }
        }
        LOG("  GPIO%02d %-8s  trans=%6d  %s\n", gpio, pins[p].name,
            transitions, transitions > 0 ? "ALIVE" : "IDLE/DEAD");
    }

    // ── 5. Camera capture test (5 attempts) ──
    LOG("\n── CAMERA CAPTURE (5 attempts) ──\n");
    if (cam_ok == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int cam_pass = 0;
        for (int i = 1; i <= 5; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                LOG("  Attempt %d: OK %zu bytes %dx%d\n", i, fb->len, fb->width, fb->height);
                // Save first successful frame
                if (cam_pass == 0) {
                    FILE *imgf = fopen("/sdcard/test_frame.rgb", "wb");
                    if (imgf) {
                        fwrite(fb->buf, 1, fb->len, imgf);
                        fclose(imgf);
                        LOG("  Saved test_frame.rgb (%zu bytes)\n", fb->len);
                    }
                }
                cam_pass++;
                esp_camera_fb_return(fb);
            } else {
                LOG("  Attempt %d: TIMEOUT\n", i);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        LOG("  Camera: %d/5 captures succeeded\n", cam_pass);
    } else {
        LOG("  Camera: SKIPPED (init failed)\n");
    }

    // Deinit camera before mic test (free DMA resources)
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── 6. Mic record 10s ──
    LOG("\n── MIC RECORD 10s ──\n");
    if (mic_ok == 0) {
        int peak = record_to_sd("/sdcard/test_10s.pcm", 10000);
        LOG("  Saved test_10s.pcm (peak=%d) %s\n", peak,
            peak > 100 ? "PASS" : "WARN: low signal");
    } else {
        LOG("  SKIPPED (mic init failed)\n");
    }

    // ── 7. Mic record 15s ──
    LOG("\n── MIC RECORD 15s ──\n");
    if (mic_ok == 0) {
        int peak = record_to_sd("/sdcard/test_15s.pcm", 15000);
        LOG("  Saved test_15s.pcm (peak=%d) %s\n", peak,
            peak > 100 ? "PASS" : "WARN: low signal");
    } else {
        LOG("  SKIPPED (mic init failed)\n");
    }

    // ── 8. SD write/read verify ──
    LOG("\n── SD READ/WRITE VERIFY ──\n");
    {
        const char *test_str = "HELIOS_SD_TEST_OK_12345";
        FILE *tf = fopen("/sdcard/test_rw.txt", "w");
        if (tf) {
            fprintf(tf, "%s", test_str);
            fclose(tf);
            char readback[64] = {0};
            tf = fopen("/sdcard/test_rw.txt", "r");
            if (tf) {
                fgets(readback, sizeof(readback), tf);
                fclose(tf);
                int match = (strcmp(readback, test_str) == 0);
                LOG("  Write/readback: %s\n", match ? "PASS" : "FAIL");
            }
        } else {
            LOG("  SD write: FAIL\n");
        }
    }

    // ── Done ──
    LOG("\n═══════════════════════════════════\n");
    LOG("  ALL TESTS COMPLETE\n");
    LOG("  Check SD card for:\n");
    LOG("    results.txt     - this report\n");
    LOG("    test_10s.pcm    - 10s mic recording\n");
    LOG("    test_15s.pcm    - 15s mic recording\n");
    LOG("    test_frame.rgb  - camera frame (if captured)\n");
    LOG("═══════════════════════════════════\n");

    if (logf) fclose(logf);

    printf("DIAG: DONE. Safe to disconnect.\n");
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
