#include "helios.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;

esp_err_t sdcard_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SD_MOSI_PIN,
        .miso_io_num   = SD_MISO_PIN,
        .sclk_io_num   = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: 0x%x", err);
        return err;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs  = SD_CS_PIN;
    slot_cfg.host_id  = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: 0x%x", err);
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        spi_bus_free(SPI2_HOST);
        s_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}

bool sdcard_is_mounted(void)
{
    return s_card != NULL;
}
