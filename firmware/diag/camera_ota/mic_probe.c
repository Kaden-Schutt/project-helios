/*
 * PDM mic probe — uses the existing mic_helios driver, consumes samples
 * in the background, and keeps a rolling RMS + peak over ~1 s windows.
 *
 * Purpose: let /mic confirm the microphone is both electrically alive
 * and picking up sound (tap the board or speak near it, watch RMS rise).
 */

#include "mic_probe.h"
#include "mic_helios.h"
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "diag_log.h"

static volatile uint32_t s_rms = 0;
static volatile uint32_t s_peak = 0;
static volatile uint64_t s_frames = 0;

static void mic_task(void *arg)
{
    const size_t N = 4096;             /* 2048 int16 samples = 128 ms @ 16 kHz */
    uint8_t *buf = heap_caps_malloc(N, MALLOC_CAP_INTERNAL);
    if (!buf) { vTaskDelete(NULL); return; }

    /* 8 × 128 ms = ~1 s rolling window. */
    uint64_t sq_window[8] = {0};
    uint16_t peak_window[8] = {0};
    uint64_t frames_window[8] = {0};
    int wi = 0;

    while (1) {
        size_t got = 0;
        esp_err_t err = mic_helios_read(buf, N, &got, 200);
        if (err != ESP_OK || got == 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        int16_t *s16 = (int16_t *)buf;
        size_t n_samples = got / 2;
        uint64_t sq_sum = 0;
        uint16_t peak = 0;
        for (size_t i = 0; i < n_samples; i++) {
            int32_t v = s16[i];
            uint32_t av = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
            if (av > peak) peak = av;
            sq_sum += (uint64_t)v * (uint64_t)v;
        }
        sq_window[wi] = sq_sum;
        peak_window[wi] = peak;
        frames_window[wi] = n_samples;
        wi = (wi + 1) & 7;

        uint64_t total_sq = 0, total_n = 0;
        uint16_t total_peak = 0;
        for (int j = 0; j < 8; j++) {
            total_sq += sq_window[j];
            total_n  += frames_window[j];
            if (peak_window[j] > total_peak) total_peak = peak_window[j];
        }
        s_rms    = total_n ? (uint32_t)sqrt((double)(total_sq / total_n)) : 0;
        s_peak   = total_peak;
        s_frames += n_samples;
    }
}

esp_err_t mic_probe_init(void)
{
    esp_err_t err = mic_helios_init();
    if (err != ESP_OK) {
        DLOG("[CAMDIAG] mic_helios_init FAILED 0x%x\n", err);
        return err;
    }
    mic_helios_start();
    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 4, NULL, 1);
    DLOG("[CAMDIAG] mic_probe_init OK (PDM 16 kHz mono, rolling ~1s window)\n");
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

uint32_t mic_probe_rms(void)        { return s_rms; }
uint32_t mic_probe_peak(void)       { return s_peak; }
uint64_t mic_probe_frames_read(void){ return s_frames; }
