#include "helios.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,      // 640x480
        .jpeg_quality = 6,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "sensor PID: 0x%04x", s->id.PID);

        // OV3660-specific tuning
        if (s->id.PID == 0x3660) {
            ESP_LOGI(TAG, "Applying OV3660 JPEG fixes...");
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -1);
            s->set_sharpness(s, 2);
        }
    }

    // Warm up: discard first few frames (sensor needs time to auto-expose)
    for (int i = 0; i < 3; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGI(TAG, "warmup frame %d: %zu bytes", i + 1, fb->len);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "warmup frame %d: timeout", i + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "camera ready (VGA JPEG Q6, OV%04x)", s ? s->id.PID : 0);
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len)
{
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

    ESP_LOGI(TAG, "captured %zu bytes fmt=%d (%dx%d)",
             s_fb->len, s_fb->format, s_fb->width, s_fb->height);
    return ESP_OK;
}

void camera_return_fb(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
}

void camera_deinit(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
    esp_camera_deinit();
    ESP_LOGI(TAG, "camera deinitialized");
}
