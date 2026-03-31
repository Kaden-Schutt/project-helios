/*
 * Speaker test firmware for XIAO ESP32S3 + MAX98357A
 * Config: 5V supply, GAIN=GND (+15dB), 8Ω speaker
 * Plays Victory Fanfare on button press with volume scaling.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "victory_pcm.h"

#define TAG "speaker_test"

// Pin mapping (current wiring)
#define I2S_LRC_PIN     5   // D4
#define I2S_BCLK_PIN    6   // D5
#define I2S_DIN_PIN     43  // D6
#define BUTTON_PIN      4   // D3

#define SAMPLE_RATE     16000

// Volume: 5V + 15dB gain means full-scale clips at ~0.90.
// Scale 0-100% maps to 0.0 - 0.90 of full int16 range.
#define MAX_SCALE       0.90f
#define VOLUME_PERCENT  60  // start at 60% — adjust to taste

static i2s_chan_handle_t tx_chan = NULL;

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    ESP_LOGI(TAG, "I2S speaker initialized (5V, +15dB, vol=%d%%)", VOLUME_PERCENT);
    return ESP_OK;
}

static void play_victory(void)
{
    // Pre-compute fixed-point scale: volume% * max_scale, as Q8
    int scale_fixed = (int)((VOLUME_PERCENT / 100.0f) * MAX_SCALE * 256.0f);

    size_t bytes_written = 0;
    size_t offset = 0;
    const size_t chunk_bytes = 1024;
    int16_t scaled_buf[512];

    i2s_channel_enable(tx_chan);
    ESP_LOGI(TAG, "SPEAKER_TEST_PLAYING_VICTORY (vol=%d%%, scale=%.2f)",
             VOLUME_PERCENT, (VOLUME_PERCENT / 100.0f) * MAX_SCALE);

    while (offset < victory_pcm_len) {
        size_t remaining = victory_pcm_len - offset;
        size_t to_process = remaining < chunk_bytes ? remaining : chunk_bytes;
        to_process &= ~1;  // align to sample boundary
        if (to_process == 0) break;

        size_t n_samples = to_process / 2;
        const int16_t *src = (const int16_t *)(victory_pcm + offset);

        for (size_t i = 0; i < n_samples; i++) {
            int32_t s = ((int32_t)src[i] * scale_fixed) >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            scaled_buf[i] = (int16_t)s;
        }

        i2s_channel_write(tx_chan, scaled_buf, to_process, &bytes_written, portMAX_DELAY);
        offset += to_process;
    }

    // Flush silence then stop
    memset(scaled_buf, 0, chunk_bytes);
    i2s_channel_write(tx_chan, scaled_buf, chunk_bytes, &bytes_written, portMAX_DELAY);
    i2s_channel_disable(tx_chan);

    ESP_LOGI(TAG, "SPEAKER_TEST_VICTORY_DONE");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Speaker Test (5V + 15dB) ===");
    ESP_LOGI(TAG, "Press button on D3 for Victory Fanfare!");

    button_init();
    speaker_init();

    int last = 0;
    while (1) {
        int now = gpio_get_level(BUTTON_PIN);
        if (now == 1 && last == 0) {
            play_victory();
        }
        last = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
