#pragma once

#include "esp_err.h"

// --- Camera (OV2640/OV3660 via DVP) ---
// Pin definitions for XIAO ESP32S3 Sense
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40  // SCCB SDA
#define CAM_PIN_SIOC    39  // SCCB SCL
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

// --- PDM Microphone (MSM261S4030H0) ---
#define MIC_CLK_PIN     42
#define MIC_DATA_PIN    41
#define MIC_SAMPLE_RATE 16000
#define MIC_BIT_DEPTH   16

// --- Camera API ---
esp_err_t camera_init(void);
esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len);
void      camera_return_fb(void);

// --- Mic API ---
esp_err_t mic_init(void);
esp_err_t mic_record(int duration_ms, uint8_t **out_buf, size_t *out_len);
void      mic_free_buf(uint8_t *buf);
