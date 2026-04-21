#pragma once

#include "esp_err.h"

/* Start the OTA HTTP server on port 80. Assumes WiFi is up. */
esp_err_t ota_http_start(void);
