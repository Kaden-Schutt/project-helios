#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Goouuu ESP32-S3-CAM pin map — matches CAMERA_MODEL_ESP32S3_EYE. */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4   /* SCCB SDA */
#define CAM_PIN_SIOC     5   /* SCCB SCL */
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

esp_err_t camera_helios_init(void);
esp_err_t camera_helios_capture(uint8_t **out_buf, size_t *out_len);
void      camera_helios_return_fb(void);
void      camera_helios_deinit(void);
bool      camera_helios_is_ready(void);
