/*
 * Quick PCLK pin test — init camera, sample PCLK for 200ms
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "helios.h"

void app_main(void)
{
    printf("\n=== PCLK Test ===\n");

    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10,
        .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14,
        .pin_d3 = 16, .pin_d2 = 18, .pin_d1 = 17, .pin_d0 = 15,
        .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QQVGA,
        .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    printf("Camera init: %s\n", err == ESP_OK ? "OK" : "FAIL");

    if (err == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            printf("Sensor: OV%04x\n", s->id.PID);
            if (s->id.PID == 0x3660) s->set_vflip(s, 1);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    // Sample PCLK and VSYNC
    int pclk_transitions = 0, vsync_transitions = 0;
    int last_pclk = gpio_get_level(13);
    int last_vsync = gpio_get_level(38);
    uint32_t start = xTaskGetTickCount();
    int samples = 0;
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)) {
        int p = gpio_get_level(13);
        int v = gpio_get_level(38);
        if (p != last_pclk) { pclk_transitions++; last_pclk = p; }
        if (v != last_vsync) { vsync_transitions++; last_vsync = v; }
        samples++;
    }

    printf("Samples: %d\n", samples);
    printf("PCLK transitions:  %d  %s\n", pclk_transitions, pclk_transitions > 0 ? "ALIVE!" : "DEAD");
    printf("VSYNC transitions: %d  %s\n", vsync_transitions, vsync_transitions > 0 ? "ALIVE" : "DEAD");

    if (pclk_transitions > 0) {
        // Try a capture
        vTaskDelay(pdMS_TO_TICKS(500));
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            printf("CAPTURE: OK! %zu bytes %dx%d\n", fb->len, fb->width, fb->height);
            esp_camera_fb_return(fb);
        } else {
            printf("CAPTURE: TIMEOUT\n");
        }
    }

    printf("=== DONE ===\n");
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
