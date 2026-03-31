#include "helios.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mic";
i2s_chan_handle_t s_rx_chan = NULL;  // non-static: accessed by ble.c for Opus streaming

esp_err_t mic_init(void)
{
    // Allocate I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel alloc failed: 0x%x", err);
        return err;
    }

    // Configure PDM RX
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_PIN,
            .din = MIC_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM RX init failed: 0x%x", err);
        return err;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel enable failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "PDM mic ready (%d Hz, 16-bit mono)", MIC_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t mic_record(int duration_ms, uint8_t **out_buf, size_t *out_len)
{
    // Calculate buffer size: sample_rate * 2 bytes * duration_sec
    size_t total_bytes = (MIC_SAMPLE_RATE * 2 * duration_ms) / 1000;
    uint8_t *buf = malloc(total_bytes);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for %zu bytes", total_bytes);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    size_t chunk = 1024;

    while (offset < total_bytes) {
        size_t to_read = (total_bytes - offset < chunk) ? (total_bytes - offset) : chunk;
        size_t bytes_read = 0;

        esp_err_t err = i2s_channel_read(s_rx_chan, buf + offset, to_read, &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read error at offset %zu: 0x%x", offset, err);
            free(buf);
            return err;
        }
        offset += bytes_read;
    }

    *out_buf = buf;
    *out_len = total_bytes;

    float duration_sec = (float)total_bytes / (MIC_SAMPLE_RATE * 2);
    ESP_LOGI(TAG, "recorded %zu bytes (%.1fs PCM s16le @ %d Hz)",
             total_bytes, duration_sec, MIC_SAMPLE_RATE);
    return ESP_OK;
}

void mic_free_buf(uint8_t *buf)
{
    free(buf);
}
