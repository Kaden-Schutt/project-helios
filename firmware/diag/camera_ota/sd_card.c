/*
 * SD card driver — microSD on XIAO Sense's onboard SPI slot.
 *
 * Used for:
 *   - WiFi credential override (/sd/wifi.conf)
 *   - OTA recovery image (/sd/recovery.signed.bin)
 *   - Diagnostic artifacts (future)
 */

#include "sd_card.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "diag_log.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sd_card_mount(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        DLOG("[SD] spi_bus_initialize err=0x%x\n", err);
        return err;
    }

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = SD_CS_PIN;
    dev_cfg.host_id = host.slot;

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        DLOG("[SD] mount failed err=0x%x (%s)\n", err, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    DLOG("[SD] mounted %s  (%s, %lluMB)\n",
         SD_MOUNT_POINT, s_card->cid.name,
         ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

void sd_card_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
}

bool sd_card_is_mounted(void) { return s_mounted; }

FILE *sd_card_open_write(const char *path)
{
    if (!s_mounted) return NULL;
    return fopen(path, "wb");
}

FILE *sd_card_open_read(const char *path)
{
    if (!s_mounted) return NULL;
    return fopen(path, "rb");
}

bool sd_card_exists(const char *path)
{
    if (!s_mounted) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

bool sd_card_unlink(const char *path)
{
    if (!s_mounted) return false;
    return unlink(path) == 0;
}

bool sd_card_rename(const char *from, const char *to)
{
    if (!s_mounted) return false;
    /* FAT rename doesn't overwrite atomically — unlink dest first. */
    unlink(to);
    return rename(from, to) == 0;
}

bool sd_card_read_wifi_conf(char *out_ssid, size_t ssid_cap,
                             char *out_psk, size_t psk_cap)
{
    wifi_entry_t one[1];
    int n = sd_card_read_wifi_list(one, 1);
    if (n < 1) return false;
    strncpy(out_ssid, one[0].ssid, ssid_cap - 1); out_ssid[ssid_cap - 1] = 0;
    strncpy(out_psk,  one[0].psk,  psk_cap - 1);  out_psk[psk_cap - 1]  = 0;
    return true;
}

/* Multi-network parser. Supports:
 *   ssid=<s>           ) legacy single
 *   psk=<p>            )
 *   ssid1=<s>          ) numbered pairs; order in file = priority
 *   psk1=<p>           )
 *   ssid2=<s> ...      )
 * An "ssidN" without matching "pskN" is skipped (security guard). */
int sd_card_read_wifi_list(wifi_entry_t *out, int max_entries)
{
    if (!s_mounted || max_entries <= 0) return 0;
    FILE *f = fopen("/sd/wifi.conf", "r");
    if (!f) return 0;

    /* Parse into a temp table indexed by number (0 = unnumbered slot). */
    wifi_entry_t tmp[16] = {0};
    bool have_ssid[16] = {0};
    bool have_psk[16]  = {0};
    int max_idx = 0;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        if (line[0] == '#' || line[0] == 0) continue;

        const char *key = NULL;   /* "ssid" or "psk" */
        const char *val = NULL;
        int idx = 0;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        val = eq + 1;

        if (strncmp(line, "ssid", 4) == 0) {
            key = "ssid";
            if (line[4]) idx = atoi(line + 4);
        } else if (strncmp(line, "psk", 3) == 0) {
            key = "psk";
            if (line[3]) idx = atoi(line + 3);
        } else {
            continue;
        }
        if (idx < 0 || idx >= (int)(sizeof(tmp) / sizeof(tmp[0]))) continue;
        if (idx > max_idx) max_idx = idx;

        if (strcmp(key, "ssid") == 0) {
            strncpy(tmp[idx].ssid, val, sizeof(tmp[idx].ssid) - 1);
            have_ssid[idx] = true;
        } else {
            strncpy(tmp[idx].psk, val, sizeof(tmp[idx].psk) - 1);
            have_psk[idx] = true;
        }
    }
    fclose(f);

    int n = 0;
    for (int i = 0; i <= max_idx && n < max_entries; i++) {
        if (have_ssid[i] && have_psk[i]) {
            out[n++] = tmp[i];
        } else if (have_ssid[i] && !have_psk[i] && strlen(tmp[i].ssid) > 0) {
            /* Allow open networks: present ssid but empty psk line missing */
            out[n] = tmp[i];
            out[n].psk[0] = 0;
            n++;
        }
    }
    if (n > 0) {
        DLOG("[SD] wifi.conf loaded %d network(s):\n", n);
        for (int i = 0; i < n; i++) {
            DLOG("[SD]   %d. \"%s\" (%s)\n", i + 1, out[i].ssid,
                 out[i].psk[0] ? "WPA2" : "open");
        }
    } else {
        DLOG("[SD] wifi.conf had no complete entries\n");
    }
    return n;
}
