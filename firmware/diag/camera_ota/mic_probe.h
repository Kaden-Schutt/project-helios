#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Start the mic driver + a background task that keeps rolling RMS/peak. */
esp_err_t mic_probe_init(void);

/* Most recent windowed stats (last ~1 s of audio). */
uint32_t mic_probe_rms(void);
uint32_t mic_probe_peak(void);
uint64_t mic_probe_frames_read(void);

/* Pause/resume — query flow suspends probe during a recording so we don't
 * fight over the single i2s DMA stream. */
void     mic_probe_suspend(void);
void     mic_probe_resume(void);
