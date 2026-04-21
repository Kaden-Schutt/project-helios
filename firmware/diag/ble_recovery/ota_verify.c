#include "ota_verify.h"
#include "ota_pubkey.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/md.h"

struct ota_verify_ctx {
    mbedtls_md_context_t md;
    int started;
};

esp_err_t ota_verify_start(ota_verify_ctx_t **out_ctx)
{
    if (!out_ctx) return ESP_ERR_INVALID_ARG;
    ota_verify_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    mbedtls_md_init(&c->md);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) { free(c); return ESP_FAIL; }
    if (mbedtls_md_setup(&c->md, info, 1) != 0) { free(c); return ESP_FAIL; }
    if (mbedtls_md_hmac_starts(&c->md, OTA_HMAC_KEY, sizeof(OTA_HMAC_KEY)) != 0) {
        mbedtls_md_free(&c->md);
        free(c);
        return ESP_FAIL;
    }
    c->started = 1;
    *out_ctx = c;
    return ESP_OK;
}

esp_err_t ota_verify_update(ota_verify_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !ctx->started) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_OK;
    return mbedtls_md_hmac_update(&ctx->md, data, len) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_verify_finish(ota_verify_ctx_t *ctx, const uint8_t tag[32])
{
    if (!ctx || !ctx->started) return ESP_ERR_INVALID_STATE;
    uint8_t computed[32];
    int rc = mbedtls_md_hmac_finish(&ctx->md, computed);
    mbedtls_md_free(&ctx->md);
    free(ctx);
    if (rc != 0) return ESP_FAIL;

    /* Constant-time compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= computed[i] ^ tag[i];
    return diff == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

void ota_verify_abort(ota_verify_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->started) mbedtls_md_free(&ctx->md);
    free(ctx);
}

esp_err_t ota_verify_one_shot(const uint8_t *buf, size_t total_len)
{
    if (!buf || total_len < 32) return ESP_ERR_INVALID_ARG;
    size_t body = total_len - 32;
    ota_verify_ctx_t *ctx = NULL;
    esp_err_t err = ota_verify_start(&ctx);
    if (err != ESP_OK) return err;
    if (body > 0) {
        err = ota_verify_update(ctx, buf, body);
        if (err != ESP_OK) { ota_verify_abort(ctx); return err; }
    }
    return ota_verify_finish(ctx, buf + body);
}
