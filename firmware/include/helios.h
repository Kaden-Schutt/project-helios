#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// --- Camera (OV2640/OV3660 via DVP) ---
// Pin definitions for XIAO ESP32S3 Sense
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40  // SCCB SDA
#define CAM_PIN_SIOC    39  // SCCB SCL
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

// --- PDM Microphone (MSM261S4030H0) ---
#define MIC_CLK_PIN     42
#define MIC_DATA_PIN    41
#define MIC_SAMPLE_RATE     16000
#define MIC_BIT_DEPTH       16
#define MIC_BLE_SAMPLE_RATE 8000   // Downsampled for BLE transport

// --- Camera API ---
esp_err_t camera_init(void);
esp_err_t camera_capture_jpeg(uint8_t **out_buf, size_t *out_len);
void      camera_return_fb(void);
void      camera_deinit(void);

// --- Mic API ---
typedef bool (*mic_keep_recording_fn)(void);
esp_err_t mic_init(void);
esp_err_t mic_record(int duration_ms, uint8_t **out_buf, size_t *out_len);
esp_err_t mic_record_while(mic_keep_recording_fn keep_going, int max_ms,
                           uint8_t **out_buf, size_t *out_len);
esp_err_t mic_record_to_file(mic_keep_recording_fn keep_going, int max_ms,
                             const char *path, size_t *out_len);
void      mic_free_buf(uint8_t *buf);

// --- SD Card (onboard Sense board slot, SPI mode) ---
#define SD_CS_PIN       21  // internal to Sense board
#define SD_SCK_PIN      7   // D8
#define SD_MISO_PIN     8   // D9
#define SD_MOSI_PIN     9   // D10
#define SD_MOUNT_POINT  "/sdcard"

// --- SD Card API ---
esp_err_t sdcard_init(void);
void      sdcard_unmount(void);
bool      sdcard_is_mounted(void);

// --- Speaker (MAX98357A via I2S STD) ---
#define SPK_LRC_PIN     5   // D4 — WS / word select
#define SPK_BCLK_PIN    6   // D5 — bit clock
#define SPK_DIN_PIN     43  // D6 — data in
#define SPK_SAMPLE_RATE     24000
#define TTS_BLE_SAMPLE_RATE 8000   // TTS sample rate for BLE transport
// Max PCM scale factor before amp clips (5V supply, GAIN=GND +15dB, 8Ω load)
// +15dB = 5.62x gain. At 5V BTL max ~4.6V peak differential.
// Full-scale would produce ~5.0V peak → clip. Safe ceiling ~0.90.
#define SPK_MAX_SCALE   0.90f

// --- Speaker API ---
esp_err_t speaker_init(void);
void      speaker_set_volume(int percent);  // 0-100
int       speaker_get_volume(void);
esp_err_t speaker_play(const uint8_t *pcm_s16, size_t len);
esp_err_t speaker_play_8k(const uint8_t *pcm_s16_8k, size_t len);
esp_err_t speaker_play_ulaw(const uint8_t *ulaw_data, size_t len, int src_sample_rate);
esp_err_t speaker_play_opus(const uint8_t *opus_data, size_t len, int src_sample_rate);
esp_err_t speaker_stop(void);

// --- BLE (NimBLE GATT Server) ---
#define BLE_DEVICE_NAME     "Helios"
#define BLE_MTU             512
#define BLE_CHUNK_SIZE      (BLE_MTU - 3)  // 509 bytes max ATT payload

// Control commands (Pi → ESP)
#define BLE_CMD_CONNECTED       0x01
#define BLE_CMD_PROCESSING      0x02
#define BLE_CMD_SET_VOLUME      0x03
#define BLE_CMD_ERROR           0x04

// Control commands (ESP → Pi)
#define BLE_CMD_BUTTON_PRESSED  0x10
#define BLE_CMD_BUTTON_RELEASED 0x11
#define BLE_CMD_PLAYBACK_DONE   0x12
#define BLE_CMD_DEVICE_STATUS   0x13

// Callbacks
typedef void (*ble_tts_chunk_cb)(const uint8_t *data, size_t len, bool is_first, bool is_last);
typedef void (*ble_control_cb)(uint8_t cmd, const uint8_t *payload, size_t payload_len);

// --- BLE API ---
esp_err_t ble_init(ble_tts_chunk_cb tts_cb, ble_control_cb ctrl_cb);
bool      ble_is_connected(void);
esp_err_t ble_send_mic_data(const uint8_t *pcm, size_t len);
esp_err_t ble_send_mic_data_from_file(const char *path, size_t file_len);
esp_err_t ble_notify_control(uint8_t cmd, const uint8_t *payload, size_t payload_len);
void      ble_start_advertising(void);

// --- Config (persistent JSON on SD card) ---
typedef struct {
    int   speaker_volume;       // 0-100, default 60
    int   button_idle_level;    // 0 or 1, -1 = auto-detect
    char  device_name[32];      // BLE advertising name
    char  tts_voice_id[64];     // Cartesia voice ID
    int   tts_sample_rate;      // 16000 or 24000
} helios_config_t;

esp_err_t config_load(helios_config_t *cfg);
esp_err_t config_save(const helios_config_t *cfg);
void      config_defaults(helios_config_t *cfg);
esp_err_t bonds_backup_to_sd(void);
esp_err_t bonds_restore_from_sd(void);
