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

void mic_free_buf(uint8_t *buf)
{
    free(buf);
}

void mic_deinit(void)
{
    if (!s_rx_chan) return;
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    ESP_LOGI(TAG, "mic deinitialized");
}
