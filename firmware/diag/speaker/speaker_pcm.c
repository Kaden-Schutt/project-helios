/*
 * Minimal PCM-only speaker driver for MAX98357A I2S amp.
 * No Opus dependency. Mono input, 16-bit signed LE, volume scaling.
 */

#include "speaker_pcm.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

static const char *TAG = "spk";
static i2s_chan_handle_t s_tx_chan = NULL;
static int s_volume = 50;         // 0-100
static int s_sample_rate = 24000;

// Amp headroom ceiling.
#define SPK_MAX_SCALE 0.90f


esp_err_t speaker_pcm_init(int sample_rate)
{
    s_sample_rate = sample_rate;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;
    chan_cfg.auto_clear    = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel alloc failed: 0x%x", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRC_PIN,
            .dout = SPK_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };

    // Force both L and R slots to carry the same mono sample so the
    // MAX98357A (L+R)/2 averaging mode outputs full amplitude
    // (default slot_mask for MONO is LEFT only, which halves output).
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S STD init failed: 0x%x", err);
        return err;
    }

    /* Enable once and leave enabled. With auto_clear=true the DMA
     * emits zeros when there's nothing to write, so idle is silent
     * and there's no BCLK start/stop click per play call. */
    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel enable failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "ready: LRC=%d BCLK=%d DIN=%d @ %d Hz, vol=%d%% (channel hot)",
             SPK_LRC_PIN, SPK_BCLK_PIN, SPK_DIN_PIN, sample_rate, s_volume);
    return ESP_OK;
}

void speaker_pcm_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "volume: %d%%", s_volume);
}

int speaker_pcm_get_volume(void)
{
    return s_volume;
}

esp_err_t speaker_pcm_play(const uint8_t *pcm_s16_mono, size_t len)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!pcm_s16_mono || len == 0) return ESP_ERR_INVALID_ARG;

    float scale = (s_volume / 100.0f) * SPK_MAX_SCALE;
    int scale_fixed = (int)(scale * 256.0f);

    const size_t chunk_samples = 512;
    int16_t scaled_buf[chunk_samples];
    size_t offset = 0;
    size_t bytes_written = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        size_t to_process = remaining < chunk_samples * 2 ? remaining : chunk_samples * 2;
        to_process &= ~1;
        if (to_process == 0) break;

        size_t n_samples = to_process / 2;
        const int16_t *src = (const int16_t *)(pcm_s16_mono + offset);

        for (size_t i = 0; i < n_samples; i++) {
            int32_t s = ((int32_t)src[i] * scale_fixed) >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            scaled_buf[i] = (int16_t)s;
        }

        i2s_channel_write(s_tx_chan, scaled_buf, to_process, &bytes_written, portMAX_DELAY);
        offset += to_process;
    }
    return ESP_OK;
}

void speaker_pcm_deinit(void)
{
    if (!s_tx_chan) return;
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
}
