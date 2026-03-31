#include "helios.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "opus.h"
#include "esp_cache.h"
#include <string.h>
#include <stdlib.h>

// 200Hz 2nd-order Butterworth high-pass biquad (Q14 fixed-point)
// fc=200Hz, fs=24000Hz, Q=0.7071. Blocks DC + sub-bass rumble.
#define HP_B0   15788
#define HP_B1  -31577
#define HP_B2   15788
#define HP_A1  -31555
#define HP_A2   15215
#define HP_SHIFT 14

static const char *TAG = "spk";
static i2s_chan_handle_t s_tx_chan = NULL;
static int s_volume = 50;  // 0-100, camera+mic killed during playback so no rail noise

esp_err_t speaker_init(void)
{
    // Use I2S_NUM_1 — mic uses I2S_NUM_0 (PDM, different mode)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel alloc failed: 0x%x", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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

void speaker_deinit(void)
{
    if (!s_tx_chan) return;
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    ESP_LOGI(TAG, "speaker deinitialized");
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

esp_err_t speaker_play_opus(const uint8_t *opus_data, size_t len, int src_sample_rate)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!opus_data || len == 0) return ESP_ERR_INVALID_ARG;

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
    int16_t *pcm_frame = heap_caps_malloc(960 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out_buf = heap_caps_malloc(960 * 6 * sizeof(int16_t), MALLOC_CAP_SPIRAM); // stereo: 2x samples
    if (!pcm_frame || !out_buf) {
        ESP_LOGE(TAG, "Failed to alloc Opus decode buffers");
        if (pcm_frame) free(pcm_frame);
        if (out_buf) free(out_buf);
        opus_decoder_destroy(dec);
        i2s_channel_disable(s_tx_chan);
        return ESP_ERR_NO_MEM;
    }
    size_t offset = 0;
    size_t frame_idx = 0;

    // 200Hz high-pass biquad state (Direct Form II Transposed)
    int32_t hp_w1 = 0, hp_w2 = 0;
    // Simple low-pass filter state: y[n] = 0.85*x[n] + 0.15*y[n-1]
    int32_t lp_prev = 0;

    while (offset + 2 <= len) {
        uint16_t frame_len = opus_data[offset] | (opus_data[offset + 1] << 8);
        offset += 2;
        if (frame_len == 0) {
            ESP_LOGI(TAG, "Opus end marker after %zu frames", frame_idx);
            break;
        }
        if (offset + frame_len > len) {
            ESP_LOGE(TAG, "Truncated Opus frame: idx=%zu len=%u offset=%zu total=%zu",
                     frame_idx, frame_len, offset, len);
            break;
        }

        int n_samples = opus_decode(dec, opus_data + offset, frame_len,
                                     pcm_frame, 960, 0);
        offset += frame_len;

        if (n_samples <= 0) {
            ESP_LOGE(TAG, "Opus decode failed: frame=%zu err=%d", frame_idx, n_samples);
            frame_idx++;
            continue;
        }

        // Biquad HP + low-pass + volume scale + stereo output
        size_t n_out = 0;
        for (int i = 0; i < n_samples; i++) {
            int32_t x = (int32_t)pcm_frame[i];

            // 200Hz high-pass biquad (Direct Form II Transposed, Q14)
            int32_t hp_out = ((int32_t)HP_B0 * x + hp_w1) >> HP_SHIFT;
            hp_w1 = (int32_t)HP_B1 * x - (int32_t)HP_A1 * hp_out + hp_w2;
            hp_w2 = (int32_t)HP_B2 * x - (int32_t)HP_A2 * hp_out;

            // Light low-pass: y = 0.85*x + 0.15*prev (fixed-point: 217/256 + 39/256)
            int32_t lp_out = (hp_out * 217 + lp_prev * 39) >> 8;
            lp_prev = lp_out;

            // Volume scale
            int32_t s = (lp_out * scale_fixed) >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            int16_t sample = (int16_t)s;
            out_buf[n_out++] = sample;  // L
            out_buf[n_out++] = sample;  // R
        }

        size_t bw = 0;
        esp_err_t write_err = i2s_channel_write(s_tx_chan, out_buf, n_out * 2, &bw, portMAX_DELAY);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed on frame %zu: 0x%x", frame_idx, write_err);
            break;
        }
        if (bw != n_out * 2) {
            ESP_LOGE(TAG, "Short I2S write on frame %zu: wrote=%zu expected=%zu",
                     frame_idx, bw, n_out * 2);
            break;
        }

        frame_idx++;
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

esp_err_t speaker_play_opus_stream(const uint8_t *buf, size_t buf_size,
                                    volatile size_t *received, volatile bool *done,
                                    int src_sample_rate)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (!buf || !received || !done) return ESP_ERR_INVALID_ARG;

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

    int16_t *pcm_frame = heap_caps_malloc(960 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out_buf = heap_caps_malloc(960 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_frame || !out_buf) {
        ESP_LOGE(TAG, "Failed to alloc decode buffers");
        if (pcm_frame) free(pcm_frame);
        if (out_buf) free(out_buf);
        opus_decoder_destroy(dec);
        i2s_channel_disable(s_tx_chan);
        return ESP_ERR_NO_MEM;
    }

    // Filter state
    int32_t hp_w1 = 0, hp_w2 = 0;
    int32_t lp_prev = 0;

    size_t read_offset = 0;
    size_t frame_idx = 0;

    while (1) {
        // Wait for frame header (2 bytes)
        while (read_offset + 2 > *received) {
            if (*done) goto stream_done;
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        // Invalidate cache before reading header
        size_t inv_start = read_offset & ~31;
        size_t inv_end = (*received + 31) & ~31;
        if (inv_end > buf_size) inv_end = buf_size;
        if (inv_end > inv_start) {
            esp_cache_msync((void *)(buf + inv_start), inv_end - inv_start,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        uint16_t frame_len = buf[read_offset] | (buf[read_offset + 1] << 8);
        read_offset += 2;

        if (frame_len == 0) {
            ESP_LOGI(TAG, "Opus end marker after %zu frames", frame_idx);
            break;
        }

        // Wait for frame data
        while (read_offset + frame_len > *received) {
            if (*done) {
                ESP_LOGW(TAG, "Truncated frame %zu: need %u, have %zu",
                         frame_idx, frame_len, *received - read_offset);
                goto stream_done;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        // Invalidate cache for frame data
        inv_start = read_offset & ~31;
        inv_end = (read_offset + frame_len + 31) & ~31;
        if (inv_end > buf_size) inv_end = buf_size;
        if (inv_end > inv_start) {
            esp_cache_msync((void *)(buf + inv_start), inv_end - inv_start,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        int n_samples = opus_decode(dec, buf + read_offset, frame_len,
                                     pcm_frame, 960, 0);
        read_offset += frame_len;

        if (n_samples <= 0) {
            ESP_LOGE(TAG, "Opus decode failed: frame=%zu err=%d", frame_idx, n_samples);
            frame_idx++;
            continue;
        }

        // Biquad HP + low-pass + volume scale + stereo output
        size_t n_out = 0;
        for (int i = 0; i < n_samples; i++) {
            int32_t x = (int32_t)pcm_frame[i];

            int32_t hp_out = ((int32_t)HP_B0 * x + hp_w1) >> HP_SHIFT;
            hp_w1 = (int32_t)HP_B1 * x - (int32_t)HP_A1 * hp_out + hp_w2;
            hp_w2 = (int32_t)HP_B2 * x - (int32_t)HP_A2 * hp_out;

            int32_t lp_out = (hp_out * 217 + lp_prev * 39) >> 8;
            lp_prev = lp_out;

            int32_t s = (lp_out * scale_fixed) >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            int16_t sample = (int16_t)s;
            out_buf[n_out++] = sample;  // L
            out_buf[n_out++] = sample;  // R
        }

        size_t bw = 0;
        i2s_channel_write(s_tx_chan, out_buf, n_out * 2, &bw, portMAX_DELAY);
        frame_idx++;
    }

stream_done:
    free(pcm_frame);
    free(out_buf);
    opus_decoder_destroy(dec);

    // Silence flush
    int16_t silence[512] = {0};
    size_t bw2 = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bw2, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    i2s_channel_disable(s_tx_chan);
    ESP_LOGI(TAG, "Opus stream done: %zu frames, %zu bytes consumed", frame_idx, read_offset);
    return ESP_OK;
}

esp_err_t speaker_stop(void)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    i2s_channel_disable(s_tx_chan);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
