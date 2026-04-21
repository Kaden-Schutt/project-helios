#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* XIAO ESP32-S3 Sense onboard microSD pins (SDSPI). */
#define SD_CLK_PIN   7
#define SD_MISO_PIN  8
#define SD_MOSI_PIN  9
#define SD_CS_PIN    21

#define SD_MOUNT_POINT "/sd"

/* Mount the SD card if one is inserted. Safe to call at boot — no-op on
 * failure, just leaves is_mounted() returning false. */
esp_err_t sd_card_mount(void);
void      sd_card_unmount(void);
bool      sd_card_is_mounted(void);

/* Convenience helpers that no-op gracefully when the card isn't mounted. */
FILE     *sd_card_open_write(const char *path);
FILE     *sd_card_open_read(const char *path);
bool      sd_card_exists(const char *path);
bool      sd_card_unlink(const char *path);
bool      sd_card_rename(const char *from, const char *to);

/* Single-network legacy form (still supported in wifi_conf). */
bool      sd_card_read_wifi_conf(char *out_ssid, size_t ssid_cap,
                                  char *out_psk,  size_t psk_cap);

/* Multi-network form. Reads /sd/wifi.conf and parses up to max_entries
 * numbered pairs (ssid1=..., psk1=..., ssid2=..., ...) AND any
 * unnumbered "ssid=/psk=" pair. Returns number of entries filled.
 * Priority order = file order. */
typedef struct {
    char ssid[33];
    char psk[65];
} wifi_entry_t;
int       sd_card_read_wifi_list(wifi_entry_t *out, int max_entries);
