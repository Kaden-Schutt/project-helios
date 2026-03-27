#include "helios.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "opus.h"
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

// µ-law decode table (built at init or first use)
static int16_t ulaw_decode_table[256];
static bool ulaw_table_ready = false;

static void build_ulaw_table(void)
{
    if (ulaw_table_ready) return;
    for (int i = 0; i < 256; i++) {
        int s = ~i;
        int sign = s & 0x80;
        int exponent = (s >> 4) & 0x07;
        int mantissa = s & 0x0F;
        int val = ((mantissa << 4) | 0x84) << exponent;
        val -= 0x84;
        ulaw_decode_table[i] = sign ? (int16_t)(-val) : (int16_t)(val);
    }
    ulaw_table_ready = true;
}

esp_err_t speaker_play_ulaw(const uint8_t *ulaw_data, size_t len, int src_sample_rate)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!ulaw_data || len == 0) return ESP_ERR_INVALID_ARG;

    build_ulaw_table();

    float scale = (s_volume / 100.0f) * SPK_MAX_SCALE;
    int scale_fixed = (int)(scale * 256.0f);
    int upsample_ratio = SPK_SAMPLE_RATE / src_sample_rate;  // e.g. 24000/8000=3

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) return err;

    // Process 256 µ-law samples at a time
    const size_t IN_CHUNK = 256;
    int16_t out_buf[IN_CHUNK * 3];  // max 3x upsample
    size_t offset = 0;
    int16_t prev_sample = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        size_t to_process = remaining < IN_CHUNK ? remaining : IN_CHUNK;
        const uint8_t *src = ulaw_data + offset;
        size_t n_out = 0;

        for (size_t i = 0; i < to_process; i++) {
            int16_t cur = ulaw_decode_table[src[i]];
            int16_t prev = (i == 0) ? prev_sample : ulaw_decode_table[src[i - 1]];

            if (upsample_ratio == 3) {
                int32_t s0 = ((int32_t)prev * 2 + (int32_t)cur) / 3;
                int32_t s1 = ((int32_t)prev + (int32_t)cur * 2) / 3;
                int32_t s2 = cur;
                s0 = (s0 * scale_fixed) >> 8;
                s1 = (s1 * scale_fixed) >> 8;
                s2 = (s2 * scale_fixed) >> 8;
                if (s0 > 32767) s0 = 32767;
                if (s0 < -32768) s0 = -32768;
                if (s1 > 32767) s1 = 32767;
                if (s1 < -32768) s1 = -32768;
                if (s2 > 32767) s2 = 32767;
                if (s2 < -32768) s2 = -32768;
                out_buf[n_out++] = (int16_t)s0;
                out_buf[n_out++] = (int16_t)s1;
                out_buf[n_out++] = (int16_t)s2;
            } else {
                // No upsample — just decode + volume
                int32_t s = ((int32_t)cur * scale_fixed) >> 8;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                out_buf[n_out++] = (int16_t)s;
            }
        }

        prev_sample = ulaw_decode_table[src[to_process - 1]];

        size_t bw = 0;
        i2s_channel_write(s_tx_chan, out_buf, n_out * 2, &bw, portMAX_DELAY);
        offset += to_process;
    }

    // Flush silence
    int16_t silence[512] = {0};
    size_t bw = 0;
    i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bw, portMAX_DELAY);

    i2s_channel_disable(s_tx_chan);
    return ESP_OK;
}

esp_err_t speaker_play_opus(const uint8_t *opus_data, size_t len, int src_sample_rate)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!opus_data || len == 0) return ESP_ERR_INVALID_ARG;

    int upsample_ratio = SPK_SAMPLE_RATE / src_sample_rate;
    float scale = (s_volume / 100.0f) * SPK_MAX_SCALE;
    int scale_fixed = (int)(scale * 256.0f);

    // Create Opus decoder
    int opus_err;
    OpusDecoder *dec = opus_decoder_create(src_sample_rate, 1, &opus_err);
    if (!dec || opus_err != OPUS_OK) {
        ESP_LOGE(TAG, "Opus decoder create failed: %d", opus_err);
        return ESP_FAIL;
    }

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        opus_decoder_destroy(dec);
        return err;
    }

    // Decode frames: [uint16_le frame_len][frame_len bytes] repeated, 0x0000 = end
    // Allocate decode buffers in PSRAM to avoid stack overflow
    int16_t *pcm_frame = heap_caps_malloc(960 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out_buf = heap_caps_malloc(960 * 3 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_frame || !out_buf) {
        ESP_LOGE(TAG, "Failed to alloc Opus decode buffers");
        if (pcm_frame) free(pcm_frame);
        if (out_buf) free(out_buf);
        opus_decoder_destroy(dec);
        i2s_channel_disable(s_tx_chan);
        return ESP_ERR_NO_MEM;
    }
    size_t offset = 0;
    int16_t prev_sample = 0;

    while (offset + 2 <= len) {
        uint16_t frame_len = opus_data[offset] | (opus_data[offset + 1] << 8);
        offset += 2;
        if (frame_len == 0) break;  // end marker
        if (offset + frame_len > len) break;  // truncated

        int n_samples = opus_decode(dec, opus_data + offset, frame_len,
                                     pcm_frame, 960, 0);
        offset += frame_len;

        if (n_samples <= 0) {
            ESP_LOGW(TAG, "Opus decode error: %d", n_samples);
            continue;
        }

        // Upsample + volume scale
        size_t n_out = 0;
        for (int i = 0; i < n_samples; i++) {
            int16_t cur = pcm_frame[i];
            int16_t prev = (i == 0) ? prev_sample : pcm_frame[i - 1];

            if (upsample_ratio == 3) {
                int32_t s0 = ((int32_t)prev * 2 + (int32_t)cur) / 3;
                int32_t s1 = ((int32_t)prev + (int32_t)cur * 2) / 3;
                int32_t s2 = cur;
                s0 = (s0 * scale_fixed) >> 8;
                s1 = (s1 * scale_fixed) >> 8;
                s2 = (s2 * scale_fixed) >> 8;
                if (s0 > 32767) s0 = 32767;
                if (s0 < -32768) s0 = -32768;
                if (s1 > 32767) s1 = 32767;
                if (s1 < -32768) s1 = -32768;
                if (s2 > 32767) s2 = 32767;
                if (s2 < -32768) s2 = -32768;
                out_buf[n_out++] = (int16_t)s0;
                out_buf[n_out++] = (int16_t)s1;
                out_buf[n_out++] = (int16_t)s2;
            } else if (upsample_ratio == 2) {
                int32_t s0 = ((int32_t)prev + (int32_t)cur) / 2;
                int32_t s1 = cur;
                s0 = (s0 * scale_fixed) >> 8;
                s1 = (s1 * scale_fixed) >> 8;
                if (s0 > 32767) s0 = 32767;
                if (s0 < -32768) s0 = -32768;
                if (s1 > 32767) s1 = 32767;
                if (s1 < -32768) s1 = -32768;
                out_buf[n_out++] = (int16_t)s0;
                out_buf[n_out++] = (int16_t)s1;
            } else {
                int32_t s = ((int32_t)cur * scale_fixed) >> 8;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                out_buf[n_out++] = (int16_t)s;
            }
        }
        prev_sample = pcm_frame[n_samples - 1];

        size_t bw = 0;
        i2s_channel_write(s_tx_chan, out_buf, n_out * 2, &bw, portMAX_DELAY);
    }

    free(pcm_frame);
    free(out_buf);
    opus_decoder_destroy(dec);

    // Generous silence flush to prevent truncation
    int16_t silence[512] = {0};
    size_t bw2 = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bw2, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    i2s_channel_disable(s_tx_chan);
    ESP_LOGI(TAG, "Opus playback done");
    return ESP_OK;
}

esp_err_t speaker_play_8k(const uint8_t *pcm_s16_8k, size_t len)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!pcm_s16_8k || len == 0) return ESP_ERR_INVALID_ARG;

    // Upsample 8kHz→24kHz (3x) with linear interpolation + volume scaling
    float scale = (s_volume / 100.0f) * SPK_MAX_SCALE;
    int scale_fixed = (int)(scale * 256.0f);

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) return err;

    // Process 256 input samples (512 bytes) → 768 output samples (1536 bytes)
    const size_t IN_CHUNK = 256;
    int16_t out_buf[IN_CHUNK * 3];
    size_t offset = 0;
    int16_t prev_sample = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        size_t to_process = remaining < (IN_CHUNK * 2) ? remaining : (IN_CHUNK * 2);
        to_process &= ~1;
        if (to_process == 0) break;

        const int16_t *src = (const int16_t *)(pcm_s16_8k + offset);
        size_t n_in = to_process / 2;
        size_t n_out = 0;

        for (size_t i = 0; i < n_in; i++) {
            int16_t cur = src[i];
            int16_t prev = (i == 0) ? prev_sample : src[i - 1];

            // 3 output samples: interpolate from prev to cur
            int32_t s0 = ((int32_t)prev * 2 + (int32_t)cur) / 3;
            int32_t s1 = ((int32_t)prev + (int32_t)cur * 2) / 3;
            int32_t s2 = cur;

            // Volume scale + clamp
            s0 = (s0 * scale_fixed) >> 8;
            s1 = (s1 * scale_fixed) >> 8;
            s2 = (s2 * scale_fixed) >> 8;
            if (s0 > 32767) s0 = 32767;
            if (s0 < -32768) s0 = -32768;
            if (s1 > 32767) s1 = 32767;
            if (s1 < -32768) s1 = -32768;
            if (s2 > 32767) s2 = 32767;
            if (s2 < -32768) s2 = -32768;

            out_buf[n_out++] = (int16_t)s0;
            out_buf[n_out++] = (int16_t)s1;
            out_buf[n_out++] = (int16_t)s2;
        }

        prev_sample = src[n_in - 1];

        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, out_buf, n_out * 2, &bytes_written, portMAX_DELAY);
        offset += to_process;
    }

    // Flush silence
    memset(out_buf, 0, 1024);
    size_t bw = 0;
    i2s_channel_write(s_tx_chan, out_buf, 1024, &bw, portMAX_DELAY);

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
