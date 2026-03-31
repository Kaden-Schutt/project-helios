#include "helios.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "helios"

void config_defaults(helios_config_t *cfg)
{
    cfg->speaker_volume = 50;
    cfg->button_idle_level = -1;  // auto-detect
}

esp_err_t config_load(helios_config_t *cfg)
{
    config_defaults(cfg);

    // Init NVS (idempotent — safe even if ble_init calls it later)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: 0x%x", err);
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No config in NVS, using defaults");
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: 0x%x", err);
        return err;
    }

    int32_t val;
    if (nvs_get_i32(h, "volume", &val) == ESP_OK)
        cfg->speaker_volume = (int)val;
    if (nvs_get_i32(h, "btn_idle", &val) == ESP_OK)
        cfg->button_idle_level = (int)val;

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded (vol=%d)", cfg->speaker_volume);
    return ESP_OK;
}

esp_err_t config_save(const helios_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: 0x%x", err);
        return err;
    }

    nvs_set_i32(h, "volume", (int32_t)cfg->speaker_volume);
    nvs_set_i32(h, "btn_idle", (int32_t)cfg->button_idle_level);
    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Config saved");
    return err;
}
