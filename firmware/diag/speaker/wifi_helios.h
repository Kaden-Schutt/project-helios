#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Initialize WiFi STA and connect to the given SSID.
// Blocks until connected or timeout (returns ESP_OK on success,
// ESP_ERR_TIMEOUT if it couldn't connect).
esp_err_t wifi_helios_init(const char *ssid, const char *password, uint32_t timeout_ms);

// True once WiFi is associated and has an IP.
bool wifi_helios_is_connected(void);

// Fill `out` with IP as dotted string (e.g. "192.168.1.42").
// `out` must be at least 16 bytes. Returns ESP_OK if connected.
esp_err_t wifi_helios_get_ip(char *out, size_t out_len);

// Disconnect and free WiFi resources.
void wifi_helios_deinit(void);
