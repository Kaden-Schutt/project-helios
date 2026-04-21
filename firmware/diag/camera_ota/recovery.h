#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Tier-2 SD recovery.
 *
 * Boot-time: read NVS "boot_counter". If we've booted ≥3 times without
 * an app ever calling mark_valid(), try to load /sd/recovery.signed.bin,
 * verify HMAC, flash into inactive OTA slot, reboot. If anything fails,
 * let the normal boot continue.
 *
 * App-time: once the app is healthy (WiFi + HTTP up), call
 * recovery_mark_app_valid() which:
 *   - calls esp_ota_mark_app_valid_cancel_rollback()
 *   - zeroes the NVS boot_counter
 *   - atomically renames /sd/recovery.staging.bin -> /sd/recovery.signed.bin
 *     so the next boot has a known-good recovery blob.
 */

esp_err_t recovery_boot_check(void);
esp_err_t recovery_mark_app_valid(void);

/* Load /sd/ble_recovery.signed.bin, verify HMAC, flash into inactive OTA
 * slot, set_boot_partition, esp_restart. Intended to be called when WiFi
 * fails to find any known SSID — jumps us into Tier-3 BLE rescue mode.
 * Returns ESP_ERR_NOT_FOUND (and does nothing) if the file isn't on SD. */
esp_err_t recovery_pivot_to_ble(void);
