#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// XIAO ESP32S3 Sense PDM mic (MSM261S4030H0)
#define MIC_CLK_PIN         42
#define MIC_DATA_PIN        41
#define MIC_SAMPLE_RATE     16000
#define MIC_BIT_DEPTH       16

// Init PDM RX on I2S_NUM_0 (mic uses 0; speaker uses 1).
// After init the channel is allocated but NOT yet enabled — call
// mic_helios_start() before reading, mic_helios_stop() to pause.
esp_err_t mic_helios_init(void);

// Enable the I2S channel. DMA starts pulling samples into ring buffers.
esp_err_t mic_helios_start(void);

// Disable the I2S channel. Call this between recording sessions to save
// power and keep the DMA buffers clean.
esp_err_t mic_helios_stop(void);

// Read up to buf_size bytes of PCM into buf. Blocks up to timeout_ms.
// On success *bytes_read is how many bytes were written.
esp_err_t mic_helios_read(uint8_t *buf, size_t buf_size,
                          size_t *bytes_read, int timeout_ms);

// Free the channel entirely.
void mic_helios_deinit(void);

bool mic_helios_is_ready(void);
