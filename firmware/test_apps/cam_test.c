/*
 * Camera resolution + BLE throughput test
 * Tests VGA/SVGA/XGA at Q2, and measures throughput with different pacing
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "helios.h"

static const char *TAG = "cam_test";

static void test_resolution(int framesize, const char *name, int quality, const char *sd_path)
{
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(300));

    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10,
        .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14,
        .pin_d3 = 16, .pin_d2 = 18, .pin_d1 = 17, .pin_d0 = 15,
        .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = framesize,
        .jpeg_quality = quality,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    if (esp_camera_init(&cfg) != ESP_OK) {
        printf("  %-20s INIT FAIL\n", name);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s && s->id.PID == 0x3660) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(200));

    fb = esp_camera_fb_get();
    if (!fb) {
        printf("  %-20s CAPTURE FAIL\n", name);
        return;
    }

    printf("  %-20s %6zu bytes %dx%d", name, fb->len, fb->width, fb->height);

    // Save to SD
    if (sd_path) {
        FILE *f = fopen(sd_path, "wb");
        if (f) { fwrite(fb->buf, 1, fb->len, f); fclose(f); }
    }

    // Send over BLE
    if (ble_is_connected()) {
        uint32_t t0 = xTaskGetTickCount();
        ble_send_jpeg(fb->buf, fb->len);
        uint32_t elapsed = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        float rate = (elapsed > 0) ? (float)fb->len / elapsed * 1000 / 1024 : 0;
        printf("  BLE: %lums %.1fKB/s", (unsigned long)elapsed, rate);
    }

    printf("\n");
    esp_camera_fb_return(fb);
}

void app_main(void)
{
    printf("\n== Camera Resolution + Throughput Test ==\n\n");

    sdcard_init();
    ble_init(NULL, NULL);

    printf("Waiting for BLE...\n");
    while (!ble_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("Connected!\n\n");

    printf("── VGA Q2 through Q5 ──\n");
    test_resolution(FRAMESIZE_VGA, "VGA Q2", 2, "/sdcard/vga_q2.jpg");
    test_resolution(FRAMESIZE_VGA, "VGA Q3", 3, "/sdcard/vga_q3.jpg");
    test_resolution(FRAMESIZE_VGA, "VGA Q4", 4, "/sdcard/vga_q4.jpg");
    test_resolution(FRAMESIZE_VGA, "VGA Q5", 5, "/sdcard/vga_q5.jpg");

    printf("\n== DONE ==\n");
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
