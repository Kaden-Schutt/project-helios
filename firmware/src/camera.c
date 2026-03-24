#include "helios.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "cam";
static camera_fb_t *s_fb = NULL;

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,          // 20 MHz
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,      // 640x480 — sweet spot from latency test
        .jpeg_quality = 12,                 // 0-63, lower = better quality
        .fb_count     = 1,                  // single buffer — no FB-OVF spam
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY, // capture only when we ask
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "sensor PID: 0x%02x", s->id.PID);
        // OV2640 = 0x26, OV3660 = 0x36, OV5640 = 0x56
    }

    ESP_LOGI(TAG, "camera ready (VGA JPEG, 2 framebuffers in PSRAM)");
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len)
{
    // Return previous framebuffer if still held
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }

    s_fb = esp_camera_fb_get();
    if (!s_fb) {
        ESP_LOGE(TAG, "capture failed");
        return ESP_FAIL;
    }

    *out_buf = s_fb->buf;
    *out_len = s_fb->len;

    ESP_LOGI(TAG, "captured %zu bytes JPEG (%dx%d)",
             s_fb->len, s_fb->width, s_fb->height);
    return ESP_OK;
}

void camera_return_fb(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
}
