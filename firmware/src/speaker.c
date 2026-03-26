#include "helios.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "spk";
static i2s_chan_handle_t s_tx_chan = NULL;
static int s_volume = 80;  // 0-100, default 80%

esp_err_t speaker_init(void)
{
    // Use I2S_NUM_1 — mic uses I2S_NUM_0 (PDM, different mode)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel alloc failed: 0x%x", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRC_PIN,
            .dout = SPK_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S STD init failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "speaker ready (LRC=%d, BCLK=%d, DIN=%d, %d Hz, vol=%d%%)",
             SPK_LRC_PIN, SPK_BCLK_PIN, SPK_DIN_PIN, SPK_SAMPLE_RATE, s_volume);
    return ESP_OK;
}

void speaker_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "volume: %d%%", s_volume);
}

int speaker_get_volume(void)
{
    return s_volume;
}

esp_err_t speaker_play(const uint8_t *pcm_s16, size_t len)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!pcm_s16 || len == 0) return ESP_ERR_INVALID_ARG;

    // Volume scaling factor: map 0-100% to 0.0-SPK_MAX_SCALE
    // SPK_MAX_SCALE accounts for amp gain headroom to prevent clipping
    float scale = (s_volume / 100.0f) * SPK_MAX_SCALE;

    // Integer math version: multiply by scale_num, shift right by 8
    int scale_fixed = (int)(scale * 256.0f);

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable failed: 0x%x", err);
        return err;
    }

    // Process in chunks, applying volume scaling
    const size_t chunk_bytes = 1024;
    int16_t scaled_buf[512];  // 1024 bytes = 512 samples
    size_t offset = 0;
    size_t bytes_written = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        size_t to_process = remaining < chunk_bytes ? remaining : chunk_bytes;
        // Align to sample boundary (2 bytes per sample)
        to_process &= ~1;
        if (to_process == 0) break;

        size_t n_samples = to_process / 2;
        const int16_t *src = (const int16_t *)(pcm_s16 + offset);

        for (size_t i = 0; i < n_samples; i++) {
            int32_t sample = ((int32_t)src[i] * scale_fixed) >> 8;
            // Clamp to int16 range
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            scaled_buf[i] = (int16_t)sample;
        }

        i2s_channel_write(s_tx_chan, scaled_buf, to_process, &bytes_written, portMAX_DELAY);
        offset += to_process;
    }

    // Flush silence so last DMA buffer clocks out
    memset(scaled_buf, 0, chunk_bytes);
    i2s_channel_write(s_tx_chan, scaled_buf, chunk_bytes, &bytes_written, portMAX_DELAY);

    i2s_channel_disable(s_tx_chan);
    return ESP_OK;
}

esp_err_t speaker_stop(void)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    i2s_channel_disable(s_tx_chan);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
