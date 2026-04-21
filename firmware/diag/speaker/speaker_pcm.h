#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define SPK_LRC_PIN   5
#define SPK_BCLK_PIN  6
#define SPK_DIN_PIN   43

esp_err_t speaker_pcm_init(int sample_rate);
void      speaker_pcm_deinit(void);
void      speaker_pcm_set_volume(int percent);
int       speaker_pcm_get_volume(void);
esp_err_t speaker_pcm_play(const uint8_t *pcm_s16_mono, size_t len);
