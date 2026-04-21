#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Default target — override at build time via -DHELIOS_STREAM_URI="ws://host:port/path". */
#ifndef HELIOS_STREAM_URI
#define HELIOS_STREAM_URI "ws://helios-pi.local:5750/ws/stream"
#endif

/* Start the always-on mic → Pi stream. Opens the WebSocket client, spawns
 * the streaming task that reads from mic_helios and pushes binary frames.
 * Idempotent. Requires WiFi and mic to be initialized already. */
esp_err_t ws_stream_start(const char *uri);

/* Stop streaming and close the WebSocket. Safe to call if not started. */
esp_err_t ws_stream_stop(void);

/* Suspend the stream task without tearing down the WS (e.g. during sleep_mode).
 * Mic audio is discarded while suspended — nothing is sent. Resume with
 * ws_stream_resume(). */
void      ws_stream_suspend(void);
void      ws_stream_resume(void);

bool      ws_stream_is_connected(void);
uint32_t  ws_stream_bytes_sent(void);
