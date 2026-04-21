/*
 * Tier-2 SD-backed OTA recovery.
 */

#include "recovery.h"
#include "sd_card.h"
#include "ota_verify.h"
#include "ota_pubkey.h"
#include "diag_log.h"

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"

#define NVS_NS          "helios"
#define NVS_KEY_COUNTER "boot_cnt"
#define RECOVERY_PATH   "/sd/recovery.signed.bin"
#define RECOVERY_STAGE  "/sd/recovery.staging.bin"
#define BLE_PATH        "/sd/ble_recovery.signed.bin"
#define BOOT_THRESHOLD  3

#ifndef FW_TAG
#define FW_TAG "debug"
#endif

static int nvs_get_counter(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t v = 0;
    nvs_get_i32(h, NVS_KEY_COUNTER, &v);
    nvs_close(h);
    return (int)v;
}

static void nvs_set_counter(int v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_COUNTER, v);
    nvs_commit(h);
    nvs_close(h);
}

/* Copy a signed .bin (firmware + 32-byte sig) into the inactive OTA
 * partition, after verifying the HMAC. Returns ESP_OK if the new slot
 * is ready to boot. Caller should then set_boot_partition + restart. */
esp_err_t recovery_apply_signed_file(const char *sd_path,
                                     const esp_partition_t **out_slot);
static esp_err_t apply_signed_bin_to_inactive_slot(const char *sd_path,
                                                   const esp_partition_t **out_slot)
{
    if (!sd_card_exists(sd_path)) return ESP_ERR_NOT_FOUND;
    FILE *f = sd_card_open_read(sd_path);
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total < (long)(OTA_SIG_LEN + 1024)) {
        DLOG("[RECOVERY] recovery file too small: %ld\n", total);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    long firmware_len = total - OTA_SIG_LEN;

    /* Pass 1: verify HMAC end-to-end. */
    ota_verify_ctx_t *vctx = NULL;
    if (ota_verify_start(&vctx) != ESP_OK) { fclose(f); return ESP_FAIL; }
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) { ota_verify_abort(vctx); fclose(f); return ESP_ERR_NO_MEM; }

    long left = firmware_len;
    while (left > 0) {
        size_t to_read = left > 4096 ? 4096 : (size_t)left;
        size_t got = fread(buf, 1, to_read, f);
        if (got == 0) { ota_verify_abort(vctx); free(buf); fclose(f); return ESP_FAIL; }
        ota_verify_update(vctx, buf, got);
        left -= got;
    }
    uint8_t sig[OTA_SIG_LEN];
    if (fread(sig, 1, OTA_SIG_LEN, f) != OTA_SIG_LEN) {
        ota_verify_abort(vctx); free(buf); fclose(f);
        return ESP_FAIL;
    }
    esp_err_t ver = ota_verify_finish(vctx, sig);
    if (ver != ESP_OK) {
        DLOG("[RECOVERY] SD recovery signature BAD — ignoring file\n");
        free(buf); fclose(f);
        return ESP_ERR_INVALID_CRC;
    }
    DLOG("[RECOVERY] signature OK, flashing %ld bytes into inactive slot\n",
         firmware_len);

    /* Pass 2: flash into inactive OTA slot. */
    fseek(f, 0, SEEK_SET);
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) { free(buf); fclose(f); return ESP_ERR_NOT_FOUND; }
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) { free(buf); fclose(f); return err; }
    left = firmware_len;
    while (left > 0) {
        size_t to_read = left > 4096 ? 4096 : (size_t)left;
        size_t got = fread(buf, 1, to_read, f);
        if (got == 0) { esp_ota_abort(h); free(buf); fclose(f); return ESP_FAIL; }
        err = esp_ota_write(h, buf, got);
        if (err != ESP_OK) { esp_ota_abort(h); free(buf); fclose(f); return err; }
        left -= got;
    }
    free(buf);
    fclose(f);

    err = esp_ota_end(h);
    if (err != ESP_OK) return err;
    *out_slot = slot;
    return ESP_OK;
}

esp_err_t recovery_boot_check(void)
{
    int counter = nvs_get_counter();
    counter++;
    nvs_set_counter(counter);
    DLOG("[RECOVERY] boot_counter=%d (threshold=%d)\n", counter, BOOT_THRESHOLD);

    if (counter < BOOT_THRESHOLD) return ESP_OK;

    DLOG("[RECOVERY] threshold hit — attempting SD-backed recovery\n");
    if (!sd_card_is_mounted()) {
        DLOG("[RECOVERY] no SD card — can't recover, continuing anyway\n");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *new_slot = NULL;
    esp_err_t err = apply_signed_bin_to_inactive_slot(RECOVERY_PATH, &new_slot);
    if (err != ESP_OK) {
        DLOG("[RECOVERY] recovery apply failed: 0x%x — continuing with current slot\n", err);
        return err;
    }
    err = esp_ota_set_boot_partition(new_slot);
    if (err != ESP_OK) {
        DLOG("[RECOVERY] set_boot_partition failed: 0x%x\n", err);
        return err;
    }
    nvs_set_counter(0);
    DLOG("[RECOVERY] rebooting into %s (recovery image)\n", new_slot->label);
    esp_restart();
    return ESP_OK;   /* not reached */
}

esp_err_t recovery_apply_signed_file(const char *sd_path,
                                     const esp_partition_t **out_slot)
{
    return apply_signed_bin_to_inactive_slot(sd_path, out_slot);
}

esp_err_t recovery_pivot_to_ble(void)
{
    if (!sd_card_is_mounted()) {
        DLOG("[RECOVERY] no SD — cannot pivot to BLE rescue\n");
        return ESP_ERR_NOT_FOUND;
    }
    if (!sd_card_exists(BLE_PATH)) {
        DLOG("[RECOVERY] %s not present — cannot pivot to BLE rescue\n", BLE_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    DLOG("[RECOVERY] WiFi unavailable — loading BLE rescue image\n");
    const esp_partition_t *new_slot = NULL;
    esp_err_t err = apply_signed_bin_to_inactive_slot(BLE_PATH, &new_slot);
    if (err != ESP_OK) {
        DLOG("[RECOVERY] apply BLE bin failed: 0x%x\n", err);
        return err;
    }
    err = esp_ota_set_boot_partition(new_slot);
    if (err != ESP_OK) {
        DLOG("[RECOVERY] set_boot_partition failed: 0x%x\n", err);
        return err;
    }
    DLOG("[RECOVERY] booting into BLE rescue (%s)\n", new_slot->label);
    esp_restart();
    return ESP_OK;
}

esp_err_t recovery_mark_app_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        DLOG("[RECOVERY] mark_valid err=0x%x (may be fine on factory boot)\n", err);
    } else {
        DLOG("[RECOVERY] app marked valid — rollback cancelled\n");
    }
    nvs_set_counter(0);

    /* Promote the staging recovery blob (written during the last OTA POST)
     * to the canonical recovery file ONLY if this running firmware is
     * tagged "prod". Debug/experimental runs don't overwrite the prod
     * safety net — recovery.signed.bin stays anchored to the last
     * known-good production image. */
    if (strcmp(FW_TAG, "prod") == 0) {
        if (sd_card_exists(RECOVERY_STAGE)) {
            if (sd_card_rename(RECOVERY_STAGE, RECOVERY_PATH)) {
                DLOG("[RECOVERY] prod build — promoted staging to %s\n", RECOVERY_PATH);
            } else {
                DLOG("[RECOVERY] failed to promote staging — keeping old recovery\n");
            }
        }
    } else {
        DLOG("[RECOVERY] tag=%s (not prod) — not touching recovery.signed.bin\n", FW_TAG);
        /* Clean up the staging file so it doesn't sit forever on SD. */
        if (sd_card_exists(RECOVERY_STAGE)) sd_card_unlink(RECOVERY_STAGE);
    }
    return ESP_OK;
}
