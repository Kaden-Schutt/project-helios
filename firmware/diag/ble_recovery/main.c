/*
 * Helios BLE Rescue
 * =================
 * Standalone recovery firmware. Advertises as "Helios-Recovery" over
 * BLE LE. Accepts a signed firmware image via GATT writes, verifies
 * HMAC-SHA256 against the embedded secret, writes to the inactive OTA
 * slot, and reboots.
 *
 * GATT service 0xFFE0:
 *   0xFFE1  fw_size   (write-with-response)   — 4-byte little-endian
 *                     total size of signed body (firmware + 32-byte sig)
 *   0xFFE2  fw_chunk  (write-without-response)— data chunks, in order
 *
 * Client flow (see scripts/ble_ota.py):
 *   write fw_size = N
 *   write fw_chunk repeatedly until N bytes delivered
 *   device auto-verifies, flashes, resets
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ota_verify.h"
#include "ota_pubkey.h"

static const char *TAG = "ble-rescue";
static uint8_t s_own_addr_type;

/* ---- Transfer state ---- */
static uint32_t s_expected_total = 0;
static uint32_t s_bytes_seen     = 0;
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_part = NULL;
static ota_verify_ctx_t *s_vctx = NULL;
static uint8_t  s_sig_buf[OTA_SIG_LEN];
static uint32_t s_sig_pos = 0;
static bool     s_in_transfer = false;

static void reset_transfer(void)
{
    if (s_vctx) { ota_verify_abort(s_vctx); s_vctx = NULL; }
    if (s_ota_handle) { esp_ota_abort(s_ota_handle); s_ota_handle = 0; }
    s_expected_total = 0;
    s_bytes_seen = 0;
    s_sig_pos = 0;
    s_in_transfer = false;
}

static int begin_transfer(uint32_t total)
{
    reset_transfer();
    if (total < OTA_SIG_LEN + 1024 || total > (1536 * 1024)) {
        ESP_LOGE(TAG, "rejecting size=%lu (out of range)", (unsigned long)total);
        return -1;
    }
    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) return -1;
    if (esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle) != ESP_OK) return -1;
    if (ota_verify_start(&s_vctx) != ESP_OK) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        return -1;
    }
    s_expected_total = total;
    s_in_transfer = true;
    ESP_LOGI(TAG, "begin transfer: %lu bytes -> %s",
             (unsigned long)total, s_ota_part->label);
    return 0;
}

static void handle_chunk(const uint8_t *data, uint16_t len)
{
    if (!s_in_transfer) { ESP_LOGW(TAG, "chunk before size"); return; }
    if (s_bytes_seen + len > s_expected_total) {
        ESP_LOGE(TAG, "oversize: %lu + %u > %lu", (unsigned long)s_bytes_seen,
                 len, (unsigned long)s_expected_total);
        reset_transfer();
        return;
    }
    uint32_t firmware_len = s_expected_total - OTA_SIG_LEN;
    uint32_t fw_in = 0, sig_in = 0;
    if (s_bytes_seen + len <= firmware_len) {
        fw_in = len;
    } else if (s_bytes_seen >= firmware_len) {
        sig_in = len;
    } else {
        fw_in = firmware_len - s_bytes_seen;
        sig_in = len - fw_in;
    }
    if (fw_in > 0) {
        ota_verify_update(s_vctx, data, fw_in);
        esp_err_t e = esp_ota_write(s_ota_handle, data, fw_in);
        if (e != ESP_OK) { ESP_LOGE(TAG, "ota_write: 0x%x", e); reset_transfer(); return; }
    }
    if (sig_in > 0) {
        uint32_t copy = sig_in;
        if (s_sig_pos + copy > OTA_SIG_LEN) copy = OTA_SIG_LEN - s_sig_pos;
        memcpy(s_sig_buf + s_sig_pos, data + fw_in, copy);
        s_sig_pos += copy;
    }
    s_bytes_seen += len;

    if (s_bytes_seen >= s_expected_total) {
        ESP_LOGI(TAG, "transfer complete — verifying");
        esp_err_t ver = ota_verify_finish(s_vctx, s_sig_buf);
        s_vctx = NULL;
        if (ver != ESP_OK) {
            ESP_LOGE(TAG, "SIGNATURE MISMATCH — aborting");
            esp_ota_abort(s_ota_handle);
            reset_transfer();
            return;
        }
        if (esp_ota_end(s_ota_handle) != ESP_OK) { reset_transfer(); return; }
        if (esp_ota_set_boot_partition(s_ota_part) != ESP_OK) { reset_transfer(); return; }
        ESP_LOGI(TAG, "signature OK — rebooting into %s in 1s", s_ota_part->label);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

/* ---- GATT service definition ---- */

#define SVC_UUID   0xFFE0
#define CHR_SIZE   0xFFE1
#define CHR_CHUNK  0xFFE2

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[260];
        if (len > sizeof(buf)) len = sizeof(buf);
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);

        if (uuid == CHR_SIZE) {
            if (len != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint32_t total = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
            return begin_transfer(total) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        } else if (uuid == CHR_CHUNK) {
            handle_chunk(buf, len);
            return 0;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CHR_SIZE),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHR_CHUNK),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- Advertising ---- */

static void start_advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status != 0) start_advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        reset_transfer();
        start_advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu changed to %d (conn=%d)",
                 event->mtu.value, event->mtu.conn_handle);
        break;
    }
    return 0;
}

static void start_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(SVC_UUID);
    fields.uuids16 = &svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields: %d", rc); return; }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start: %d", rc);
    else         ESP_LOGI(TAG, "advertising as \"Helios-Recovery\"");
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_advertise();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "Helios BLE Rescue — running on %s", running ? running->label : "?");
    ESP_LOGI(TAG, "heap internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "==============================================");

    /* Mark this boot valid immediately. This firmware is tiny and its
     * only job is to accept a signed main image — if BLE init works, it
     * works. Don't let rollback drag us out. */
    esp_ota_mark_app_valid_cancel_rollback();

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);
    ble_svc_gap_device_name_set("Helios-Recovery");
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
