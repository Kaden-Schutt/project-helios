/*
 * PDM mic driver for Helios WiFi test.
 * Adapted from firmware/src/mic.c — plus a read() function the original
 * didn't need (BLE version streamed from inside ble.c's recorder).
 */

#include "mic_helios.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_started = false;

esp_err_t mic_helios_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;  // ~64ms per frame @ 16kHz

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel alloc failed: 0x%x", err);
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_PIN,
            .din = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM RX init failed: 0x%x", err);
        return err;
    }

    // Enable channel once at init and leave it running. Enable/disable
    // cycles on the PDM RX have shown to miss samples — keep DMA hot.
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel enable failed: 0x%x", err);
        return err;
    }
    s_started = true;

    ESP_LOGI(TAG, "mic initialized (%d Hz, 16-bit mono, clk=%d din=%d, DMA on)",
             MIC_SAMPLE_RATE, MIC_CLK_PIN, MIC_DATA_PIN);
    return ESP_OK;
}

// Flush stale samples that accumulated in DMA while we weren't reading.
// Call right before starting a new recording so the recording starts "fresh".
esp_err_t mic_helios_start(void)
{
    if (!s_rx_chan || !s_started) return ESP_ERR_INVALID_STATE;
    uint8_t drop[4096];
    size_t n = 0;
    // Drain for up to ~100ms of stale buffer
    for (int i = 0; i < 8; i++) {
        esp_err_t err = i2s_channel_read(s_rx_chan, drop, sizeof(drop), &n, 0);
        if (err != ESP_OK || n == 0) break;
    }
    return ESP_OK;
}

// No-op in always-on mode; the channel stays enabled.
esp_err_t mic_helios_stop(void)
{
    return ESP_OK;
}

esp_err_t mic_helios_read(uint8_t *buf, size_t buf_size,
                          size_t *bytes_read, int timeout_ms)
{
    if (!s_rx_chan || !s_started) return ESP_ERR_INVALID_STATE;
    if (!buf || buf_size == 0 || !bytes_read) return ESP_ERR_INVALID_ARG;

    return i2s_channel_read(s_rx_chan, buf, buf_size, bytes_read,
                            pdMS_TO_TICKS(timeout_ms));
}

void mic_helios_deinit(void)
{
    if (!s_rx_chan) return;
    if (s_started) {
        i2s_channel_disable(s_rx_chan);
        s_started = false;
    }
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    ESP_LOGI(TAG, "mic deinit");
}

bool mic_helios_is_ready(void)
{
    return s_rx_chan != NULL;
}
