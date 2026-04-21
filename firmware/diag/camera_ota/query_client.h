#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Default Pi URL — override at build time via -DHELIOS_PI_URL=... */
#ifndef HELIOS_PI_URL
#define HELIOS_PI_URL "http://192.168.68.103:5750"
#endif

#define QUERY_MAX_S        15       /* hard cap on recording duration */
#define QUERY_SAMPLE_RATE  16000    /* matches mic_helios */

/* Start the button-hold-driven query task. Watches button_is_holding(); on
 * hold_start it suspends mic_probe and drains mic PCM into a PSRAM buffer
 * until hold_end. On release it POSTs a fresh JPEG to /photo/upload and the
 * recorded PCM to /query. Pi handles STT/LLM/TTS and plays audio locally. */
esp_err_t query_client_init(void);
