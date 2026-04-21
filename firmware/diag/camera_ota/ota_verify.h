#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Incremental HMAC-SHA256 verifier: start, feed body bytes, finish by
 * comparing computed tag against the 32-byte tag that arrived at the
 * end of the signed buffer. All constant-time on the compare. */

typedef struct ota_verify_ctx ota_verify_ctx_t;

esp_err_t ota_verify_start(ota_verify_ctx_t **out_ctx);
esp_err_t ota_verify_update(ota_verify_ctx_t *ctx, const uint8_t *data, size_t len);
/* Returns ESP_OK if tag matches the HMAC over all update() data. */
esp_err_t ota_verify_finish(ota_verify_ctx_t *ctx, const uint8_t tag[32]);
void      ota_verify_abort(ota_verify_ctx_t *ctx);

/* One-shot verify over a whole buffer; buf contains firmware followed by
 * 32-byte tag (so last 32 bytes are the signature). */
esp_err_t ota_verify_one_shot(const uint8_t *buf, size_t total_len);
