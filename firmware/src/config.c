#include "helios.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "config";

#define CONFIG_PATH SD_MOUNT_POINT "/helios_cfg.json"
#define BONDS_PATH  SD_MOUNT_POINT "/ble_bonds.bin"

void config_defaults(helios_config_t *cfg)
{
    cfg->speaker_volume = 60;
    cfg->button_idle_level = -1;  // auto-detect
    strncpy(cfg->device_name, BLE_DEVICE_NAME, sizeof(cfg->device_name));
    strncpy(cfg->tts_voice_id, "f786b574-daa5-4673-aa0c-cbe3e8534c02", sizeof(cfg->tts_voice_id));
    cfg->tts_sample_rate = 24000;
}

esp_err_t config_load(helios_config_t *cfg)
{
    config_defaults(cfg);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Config parse failed");
        return ESP_FAIL;
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "speaker_volume")) && cJSON_IsNumber(item))
        cfg->speaker_volume = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "button_idle_level")) && cJSON_IsNumber(item))
        cfg->button_idle_level = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "device_name")) && cJSON_IsString(item))
        strncpy(cfg->device_name, item->valuestring, sizeof(cfg->device_name) - 1);
    if ((item = cJSON_GetObjectItem(root, "tts_voice_id")) && cJSON_IsString(item))
        strncpy(cfg->tts_voice_id, item->valuestring, sizeof(cfg->tts_voice_id) - 1);
    if ((item = cJSON_GetObjectItem(root, "tts_sample_rate")) && cJSON_IsNumber(item))
        cfg->tts_sample_rate = item->valueint;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded (vol=%d)", cfg->speaker_volume);
    return ESP_OK;
}

esp_err_t config_save(const helios_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "speaker_volume", cfg->speaker_volume);
    cJSON_AddNumberToObject(root, "button_idle_level", cfg->button_idle_level);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddStringToObject(root, "tts_voice_id", cfg->tts_voice_id);
    cJSON_AddNumberToObject(root, "tts_sample_rate", cfg->tts_sample_rate);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) { free(json); return ESP_FAIL; }
    fputs(json, f);
    fclose(f);
    free(json);

    ESP_LOGI(TAG, "Config saved");
    return ESP_OK;
}

// Bond backup is a simplified approach: save/restore the NVS "nimble_bond" namespace
// For now, just track if bonds exist and log it.
// Full NVS serialization is complex; defer to a simple "has bonded" flag.
esp_err_t bonds_backup_to_sd(void)
{
    // Write a marker file so we know bonding happened
    FILE *f = fopen(BONDS_PATH, "w");
    if (!f) return ESP_FAIL;
    fprintf(f, "bonded");
    fclose(f);
    ESP_LOGI(TAG, "Bond marker saved to SD");
    return ESP_OK;
}

esp_err_t bonds_restore_from_sd(void)
{
    FILE *f = fopen(BONDS_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No bond backup on SD");
        return ESP_ERR_NOT_FOUND;
    }
    fclose(f);
    // NVS bonds are managed by NimBLE automatically.
    // If NVS was erased (reflash), the Pi will need to re-pair.
    // The marker file tells us bonding was previously successful.
    ESP_LOGI(TAG, "Bond marker found on SD (re-pair may be needed after reflash)");
    return ESP_OK;
}
