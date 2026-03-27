#include "helios.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_pdm.h"
#include "opus.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble";

// µ-law encode: 16-bit signed PCM → 8-bit µ-law
static uint8_t pcm_to_ulaw(int16_t sample)
{
    const int BIAS = 0x84;
    const int MAX = 0x7FFF;
    int sign, exponent, mantissa;
    uint8_t ulawbyte;

    sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > MAX) sample = MAX;
    sample += BIAS;

    exponent = 7;
    for (int expMask = 0x4000; (sample & expMask) == 0 && exponent > 0; exponent--, expMask >>= 1) {}

    mantissa = (sample >> (exponent + 3)) & 0x0F;
    ulawbyte = ~(sign | (exponent << 4) | mantissa);
    return ulawbyte;
}

// --- State ---
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mic_chr_handle;
static uint16_t s_ctrl_chr_handle;
static bool s_mic_notify_enabled = false;
static bool s_ctrl_notify_enabled = false;
static ble_tts_chunk_cb s_tts_cb = NULL;
static ble_control_cb s_ctrl_cb = NULL;
static SemaphoreHandle_t s_notify_sem = NULL;
static bool s_tts_first_write = true;
static uint8_t s_own_addr_type = 0;

// --- UUIDs (128-bit, little-endian) ---
// Service: 87654321-4321-4321-4321-cba987654321
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0x21, 0x43,
    0x21, 0x43, 0x21, 0x43, 0x21, 0x43, 0x65, 0x87);

// Mic TX: ...4322
static const ble_uuid128_t mic_uuid = BLE_UUID128_INIT(
    0x22, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0x21, 0x43,
    0x21, 0x43, 0x21, 0x43, 0x21, 0x43, 0x65, 0x87);

// Speaker RX: ...4323
static const ble_uuid128_t spk_uuid = BLE_UUID128_INIT(
    0x23, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0x21, 0x43,
    0x21, 0x43, 0x21, 0x43, 0x21, 0x43, 0x65, 0x87);

// Control: ...4324
static const ble_uuid128_t ctrl_uuid = BLE_UUID128_INIT(
    0x24, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0x21, 0x43,
    0x21, 0x43, 0x21, 0x43, 0x21, 0x43, 0x65, 0x87);

// --- Forward declarations ---
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_on_sync(void);
static void ble_host_task(void *param);

// --- GATT access callbacks ---

static int mic_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Mic is notify-only; no read/write needed from central
    return 0;
}

static int spk_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (!s_tts_cb) return 0;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

    if (len == 0) {
        // End of stream
        s_tts_cb(NULL, 0, false, true);
        s_tts_first_write = true;
        return 0;
    }

    uint8_t buf[BLE_CHUNK_SIZE];
    uint16_t copy_len = len > sizeof(buf) ? sizeof(buf) : len;
    os_mbuf_copydata(ctxt->om, 0, copy_len, buf);

    s_tts_cb(buf, copy_len, s_tts_first_write, false);
    s_tts_first_write = false;
    return 0;
}

static int ctrl_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1 && s_ctrl_cb) {
            uint8_t buf[64];
            uint16_t copy_len = len > sizeof(buf) ? sizeof(buf) : len;
            os_mbuf_copydata(ctxt->om, 0, copy_len, buf);
            s_ctrl_cb(buf[0], buf + 1, copy_len - 1);
        }
    }
    return 0;
}

// --- GATT service definition (inline compound literals per NimBLE convention) ---
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &mic_uuid.u,
                .access_cb = mic_chr_access,
                .val_handle = &s_mic_chr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &spk_uuid.u,
                .access_cb = spk_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &ctrl_uuid.u,
                .access_cb = ctrl_chr_access,
                .val_handle = &s_ctrl_chr_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

// --- GAP event handler ---
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_tts_first_write = true;
            ESP_LOGI(TAG, "Connected (handle=%d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_mic_notify_enabled = false;
        s_ctrl_notify_enabled = false;
        ble_start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_mic_chr_handle) {
            s_mic_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Mic notify %s", s_mic_notify_enabled ? "ON" : "OFF");
        } else if (event->subscribe.attr_handle == s_ctrl_chr_handle) {
            s_ctrl_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Ctrl notify %s", s_ctrl_notify_enabled ? "ON" : "OFF");
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption changed: status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            bonds_backup_to_sd();
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Delete old bond and accept new pairing
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return 0; // accept
    }
    }
    return 0;
}

// --- Advertising ---
void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "Advertising as \"%s\"", BLE_DEVICE_NAME);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

// --- Host sync callback ---
static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE host synced (addr_type=%d), starting advertising...", s_own_addr_type);
    ble_start_advertising();
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- Public API ---

esp_err_t ble_init(ble_tts_chunk_cb tts_cb, ble_control_cb ctrl_cb)
{
    s_tts_cb = tts_cb;
    s_ctrl_cb = ctrl_cb;

    // Init NVS (needed for BLE bonding storage)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Try restoring bonds from SD
    bonds_restore_from_sd();

    // Init NimBLE
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: 0x%x", err);
        return err;
    }

    // Register GATT services BEFORE configuring host
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_LOGI(TAG, "gatt_svcs[0].type=%d uuid=%p chars=%p",
             gatt_svcs[0].type, gatt_svcs[0].uuid, gatt_svcs[0].characteristics);
    int rc = ble_gatts_count_cfg(gatt_svcs);
    ESP_LOGI(TAG, "GATT count rc=%d", rc);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    ESP_LOGI(TAG, "GATT add: %d", rc);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT add failed: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    // Configure security (Just Works pairing)
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sync_cb = ble_on_sync;

    // Flow control semaphore (allow 10 outstanding notifications)
    s_notify_sem = xSemaphoreCreateCounting(10, 10);

    // Start host task
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE initialized");
    return ESP_OK;
}

bool ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

esp_err_t ble_send_mic_data(const uint8_t *pcm, size_t len)
{
    if (!ble_is_connected() || !s_mic_notify_enabled) return ESP_ERR_INVALID_STATE;

    // Send 4-byte size header
    uint8_t hdr[4] = {
        (len >>  0) & 0xFF, (len >>  8) & 0xFF,
        (len >> 16) & 0xFF, (len >> 24) & 0xFF,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, 4);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    // Send data chunks
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > BLE_CHUNK_SIZE) chunk = BLE_CHUNK_SIZE;

        om = ble_hs_mbuf_from_flat(pcm + sent, chunk);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                // BLE stack congested — wait and retry
                os_mbuf_free_chain(om);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (rc != 0) {
                ESP_LOGW(TAG, "Notify failed at %zu/%zu: %d", sent, len, rc);
                os_mbuf_free_chain(om);
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }
        sent += chunk;
        // Pace notifications to avoid congestion
        if (sent % (BLE_CHUNK_SIZE * 4) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Zero-length end marker
    om = ble_hs_mbuf_from_flat(NULL, 0);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    ESP_LOGI(TAG, "Sent %zu bytes mic PCM", sent);
    return ESP_OK;
}

esp_err_t ble_send_mic_data_from_file(const char *path, size_t file_len)
{
    if (!ble_is_connected() || !s_mic_notify_enabled) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;

    // µ-law encoded size = half of original (16-bit → 8-bit)
    size_t ulaw_total = file_len / 2;

    // Send 4-byte size header with µ-law encoded size
    uint8_t hdr[4] = {
        (ulaw_total >>  0) & 0xFF, (ulaw_total >>  8) & 0xFF,
        (ulaw_total >> 16) & 0xFF, (ulaw_total >> 24) & 0xFF,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, 4);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    // Read 16kHz s16le from file, µ-law encode to 8-bit, send over BLE
    uint8_t read_buf[1024];           // 512 samples of s16le
    uint8_t send_buf[BLE_CHUNK_SIZE];
    size_t send_pos = 0;
    size_t sent = 0;
    size_t file_read = 0;

    while (file_read < file_len) {
        size_t to_read = file_len - file_read;
        if (to_read > sizeof(read_buf)) to_read = sizeof(read_buf);
        to_read &= ~1;  // align to sample boundary
        size_t n = fread(read_buf, 1, to_read, f);
        if (n == 0) break;
        file_read += n;

        // µ-law encode with DC offset removal: 2 bytes in → 1 byte out
        int16_t *samples = (int16_t *)read_buf;
        size_t n_samples = n / 2;
        // Compute DC offset of this chunk
        int32_t sum = 0;
        for (size_t i = 0; i < n_samples; i++) sum += samples[i];
        int16_t dc = (int16_t)(sum / (int32_t)n_samples);

        for (size_t i = 0; i < n_samples; i++) {
            int16_t centered = samples[i] - dc;
            send_buf[send_pos++] = pcm_to_ulaw(centered);

            if (send_pos >= BLE_CHUNK_SIZE) {
                int retries = 0;
                while (retries < 50) {
                    om = ble_hs_mbuf_from_flat(send_buf, send_pos);
                    if (!om) { vTaskDelay(pdMS_TO_TICKS(20)); retries++; continue; }
                    int rc = ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);
                    if (rc == 0) break;
                    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                        vTaskDelay(pdMS_TO_TICKS(20)); retries++; continue;
                    }
                    ESP_LOGW(TAG, "Notify error at %zu: %d", sent, rc);
                    break;
                }
                sent += send_pos;
                send_pos = 0;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }

    // Flush remaining
    if (send_pos > 0) {
        int retries = 0;
        while (retries < 50) {
            om = ble_hs_mbuf_from_flat(send_buf, send_pos);
            if (!om) { vTaskDelay(pdMS_TO_TICKS(20)); retries++; continue; }
            int rc = ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);
            if (rc == 0) break;
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                vTaskDelay(pdMS_TO_TICKS(20)); retries++; continue;
            }
            break;
        }
        sent += send_pos;
    }

    // End marker
    vTaskDelay(pdMS_TO_TICKS(100));
    om = ble_hs_mbuf_from_flat(NULL, 0);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    fclose(f);
    ESP_LOGI(TAG, "Sent %zu bytes µ-law (from %zu bytes PCM) from %s", sent, file_len, path);
    return ESP_OK;
}

esp_err_t ble_stream_mic_opus(mic_keep_recording_fn keep_going, int max_ms)
{
    if (!ble_is_connected() || !s_mic_notify_enabled) return ESP_ERR_INVALID_STATE;

    // Create Opus encoder
    int opus_err;
    OpusEncoder *enc = opus_encoder_create(MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        ESP_LOGE(TAG, "Opus encoder create failed: %d", opus_err);
        return ESP_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));  // low complexity for real-time

    // I2S mic channel — get from mic.c (it's already initialized and enabled)
    extern i2s_chan_handle_t s_rx_chan;  // from mic.c

    // 20ms frame at 16kHz = 320 samples = 640 bytes PCM
    const int frame_samples = MIC_SAMPLE_RATE / 50;  // 320
    const int frame_bytes = frame_samples * 2;         // 640
    int16_t *pcm_frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM);
    uint8_t *opus_out = malloc(256);  // max Opus frame size
    if (!pcm_frame || !opus_out) {
        ESP_LOGE(TAG, "Alloc failed for Opus stream buffers");
        if (pcm_frame) free(pcm_frame);
        if (opus_out) free(opus_out);
        opus_encoder_destroy(enc);
        return ESP_ERR_NO_MEM;
    }

    // Send a "start" marker: 4 bytes with 0xFFFFFFFF to signal Opus stream mode
    uint8_t start_marker[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(start_marker, 4);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    size_t frames_sent = 0;
    size_t total_opus_bytes = 0;
    size_t min_bytes = (MIC_SAMPLE_RATE * 2 * 500) / 1000;  // 500ms minimum
    size_t total_pcm_read = 0;
    size_t max_bytes = (MIC_SAMPLE_RATE * 2 * max_ms) / 1000;

    ESP_LOGI(TAG, "Opus mic streaming started (20ms frames, 16kbps)");

    while (total_pcm_read < max_bytes) {
        // Check button after minimum recording time
        if (total_pcm_read >= min_bytes && !keep_going()) break;

        // Read one 20ms frame from I2S
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, pcm_frame, frame_bytes,
                                          &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read < (size_t)frame_bytes) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        total_pcm_read += bytes_read;

        // Remove DC offset from frame
        int32_t sum = 0;
        for (int i = 0; i < frame_samples; i++) sum += pcm_frame[i];
        int16_t dc = (int16_t)(sum / frame_samples);
        for (int i = 0; i < frame_samples; i++) pcm_frame[i] -= dc;

        // Opus encode
        int opus_len = opus_encode(enc, pcm_frame, frame_samples, opus_out, 256);
        if (opus_len < 0) {
            ESP_LOGW(TAG, "Opus encode error: %d", opus_len);
            continue;
        }

        // Send as BLE notification: [uint16_le len][opus data]
        uint8_t notify_buf[260];
        notify_buf[0] = opus_len & 0xFF;
        notify_buf[1] = (opus_len >> 8) & 0xFF;
        memcpy(notify_buf + 2, opus_out, opus_len);

        int retries = 0;
        while (retries < 20) {
            om = ble_hs_mbuf_from_flat(notify_buf, 2 + opus_len);
            if (!om) { vTaskDelay(pdMS_TO_TICKS(5)); retries++; continue; }
            int rc = ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);
            if (rc == 0) break;
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                vTaskDelay(pdMS_TO_TICKS(5)); retries++; continue;
            }
            break;
        }

        frames_sent++;
        total_opus_bytes += opus_len;
    }

    // End marker
    vTaskDelay(pdMS_TO_TICKS(50));
    om = ble_hs_mbuf_from_flat(NULL, 0);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_mic_chr_handle, om);

    free(pcm_frame);
    free(opus_out);
    opus_encoder_destroy(enc);

    float duration = (float)total_pcm_read / (MIC_SAMPLE_RATE * 2);
    ESP_LOGI(TAG, "Opus stream done: %zu frames, %zu bytes Opus (%.1fs audio, %.1f:1)",
             frames_sent, total_opus_bytes, duration,
             total_pcm_read > 0 ? (float)total_pcm_read / total_opus_bytes : 0);
    return ESP_OK;
}

esp_err_t ble_notify_control(uint8_t cmd, const uint8_t *payload, size_t payload_len)
{
    if (!ble_is_connected() || !s_ctrl_notify_enabled) return ESP_ERR_INVALID_STATE;

    uint8_t buf[64];
    buf[0] = cmd;
    if (payload && payload_len > 0 && payload_len < sizeof(buf) - 1) {
        memcpy(buf + 1, payload, payload_len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, 1 + payload_len);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_ctrl_chr_handle, om);
    return ESP_OK;
}
