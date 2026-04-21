/*
 * Helios SPEAKER DIAGNOSTIC v3 — OTA-enabled
 * ==========================================
 * On boot:
 *   1. Print chip/heap/reset banner.
 *   2. Join "Schutt Home" WiFi (creds in wifi_credentials.h, gitignored).
 *   3. Register mDNS hostname (helios-diag.local).
 *   4. Start OTA HTTP server on :80. Endpoints:
 *         GET  /info  — status JSON
 *         POST /ota   — firmware.bin body, flashes + reboots.
 *   5. Run the speaker TTS test sweep (same sequence as v2).
 *   6. Idle forever. OTA server keeps accepting uploads.
 *
 * Once battery-only, push new firmware from Mac:
 *     curl --data-binary @firmware.bin http://helios-diag.local/ota
 * or, by IP:
 *     curl --data-binary @firmware.bin http://<ip>/ota
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "mdns.h"

#include "speaker_pcm.h"
#include "tts_clips.h"
#include "wifi_helios.h"
#include "wifi_credentials.h"
#include "ota.h"
#include "diag_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Loud synthetic tone — unambiguous "is any audio coming out?" check. */
static int16_t *gen_sine(float freq, float amp_0_1, float seconds, size_t *bytes_out)
{
    const int SR = 24000;
    size_t n = (size_t)(SR * seconds);
    *bytes_out = n * 2;
    int16_t *buf = (int16_t *)heap_caps_malloc(*bytes_out, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    double phase = 0.0;
    double step = 2.0 * M_PI * (double)freq / (double)SR;
    int16_t peak = (int16_t)(amp_0_1 * 30000.0f);
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t)(sin(phase) * (double)peak);
        phase += step;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    return buf;
}

static const int SAMPLE_RATE = 24000;

static const char *rr_name(esp_reset_reason_t rr)
{
    switch (rr) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        default:                return "unknown";
    }
}

static void banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    esp_reset_reason_t rr = esp_reset_reason();

    const esp_partition_t *running = esp_ota_get_running_partition();

    DLOG("\n[SPKDIAG] ================================================\n");
    DLOG("[SPKDIAG] Helios Speaker Diagnostic v3 — OTA enabled\n");
    DLOG("[SPKDIAG] ================================================\n");
    DLOG("[SPKDIAG] chip=%s cores=%d rev=%d flash=%u MB\n",
           info.model == CHIP_ESP32S3 ? "ESP32-S3" : "unknown",
           info.cores, info.revision, (unsigned)(flash_size / (1024 * 1024)));
    DLOG("[SPKDIAG] reset_reason=%d (%s)%s\n", rr, rr_name(rr),
           rr == ESP_RST_BROWNOUT ? "  <-- BROWNOUT" : "");
    DLOG("[SPKDIAG] running_partition=%s\n",
           running ? running->label : "?");
    DLOG("[SPKDIAG] heap internal=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    DLOG("[SPKDIAG] pins LRC=%d BCLK=%d DIN=%d @ %d Hz mono, I2S1\n",
           SPK_LRC_PIN, SPK_BCLK_PIN, SPK_DIN_PIN, SAMPLE_RATE);
    DLOG("[SPKDIAG] baked clips: SHORT=%u bytes (%.2fs), MEDIUM=%u bytes (%.2fs)\n",
           (unsigned)CLIP_SHORT_LEN,  CLIP_SHORT_LEN  / 2.0 / SAMPLE_RATE,
           (unsigned)CLIP_MEDIUM_LEN, CLIP_MEDIUM_LEN / 2.0 / SAMPLE_RATE);
}

static void play(const char *name, const uint8_t *pcm, uint32_t len)
{
    unsigned expected_us =
        (unsigned)((uint64_t)(len / 2) * 1000000ULL / (uint64_t)SAMPLE_RATE);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = speaker_pcm_play(pcm, len);
    int64_t dt = esp_timer_get_time() - t0;
    double drift = 100.0 * ((double)dt - (double)expected_us) / (double)expected_us;
    DLOG("[SPKDIAG] %s: err=0x%x bytes=%u took_us=%lld expected=%u drift=%.2f%%\n",
           name, err, (unsigned)len, (long long)dt, expected_us, drift);
}

static void silent_gap(const char *label, int seconds)
{
    DLOG("[SPKDIAG] ---- %s: %ds silence ----\n", label, seconds);
    for (int s = 0; s < seconds; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        DLOG("[SPKDIAG] %s t=%ds\n", label, s + 1);
    }
}

static esp_err_t mdns_up(const char *ip_str)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    DLOG("[SPKDIAG] mDNS: %s.local -> %s\n", MDNS_HOSTNAME, ip_str);
    return ESP_OK;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    dlog_init();
    banner();

    /* ---------- WiFi + mDNS + OTA server ---------- */
    DLOG("[SPKDIAG] Connecting WiFi SSID=\"%s\" ...\n", WIFI_SSID);
    esp_err_t wer = wifi_helios_init(WIFI_SSID, WIFI_PASSWORD, 20000);
    if (wer == ESP_OK) {
        char ip[32] = "?";
        wifi_helios_get_ip(ip, sizeof(ip));
        DLOG("[SPKDIAG] WiFi OK, IP=%s\n", ip);
        mdns_up(ip);
        esp_err_t oer = ota_http_start();
        DLOG("[SPKDIAG] ota_http_start err=0x%x\n", oer);
        DLOG("[SPKDIAG] Push new fw:  curl --data-binary @firmware.bin "
               "http://%s.local/ota\n", MDNS_HOSTNAME);
    } else {
        DLOG("[SPKDIAG] WiFi FAILED (err=0x%x). OTA disabled — USB reflash needed.\n", wer);
    }

    /* ---------- Speaker sweep ---------- */
    esp_err_t err = speaker_pcm_init(SAMPLE_RATE);
    DLOG("[SPKDIAG] speaker_pcm_init err=0x%x\n", err);
    if (err != ESP_OK) {
        DLOG("[SPKDIAG] speaker init failed. Idle (OTA still up if WiFi ok).\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(60000));
    }
    speaker_pcm_set_volume(40);

    DLOG("[SPKDIAG] ---- P0 IDLE 3s ----\n");
    silent_gap("P0_IDLE", 3);

    /* Gentle tone prologue — same volume as TTS, just pure sines so you
     * can clearly tell "audio path is working" without being startled. */
    {
        size_t n;
        int16_t *buf;
        speaker_pcm_set_volume(40);

        DLOG("[SPKDIAG] ---- T1 gentle 1kHz 1.5s @ vol 40 ----\n");
        buf = gen_sine(1000.0f, 0.35f, 1.5f, &n);
        if (buf) { speaker_pcm_play((uint8_t *)buf, n); heap_caps_free(buf); }
        DLOG("[SPKDIAG] T1 done\n");

        vTaskDelay(pdMS_TO_TICKS(600));

        DLOG("[SPKDIAG] ---- T2 gentle 440Hz 1.5s @ vol 40 ----\n");
        buf = gen_sine(440.0f, 0.35f, 1.5f, &n);
        if (buf) { speaker_pcm_play((uint8_t *)buf, n); heap_caps_free(buf); }
        DLOG("[SPKDIAG] T2 done\n");
    }

    vTaskDelay(pdMS_TO_TICKS(800));

    DLOG("[SPKDIAG] ---- P1 CLIP_SHORT ----\n");
    play("P1_SHORT", CLIP_SHORT, CLIP_SHORT_LEN);

    silent_gap("P2_GAP_5s", 5);

    DLOG("[SPKDIAG] ---- P3 CLIP_SHORT repeat ----\n");
    play("P3_SHORT", CLIP_SHORT, CLIP_SHORT_LEN);

    silent_gap("P4_GAP_15s", 15);

    DLOG("[SPKDIAG] ---- P5 CLIP_MEDIUM ----\n");
    play("P5_MEDIUM", CLIP_MEDIUM, CLIP_MEDIUM_LEN);

    DLOG("[SPKDIAG] ---- P6 back-to-back SHORT+SHORT ----\n");
    int64_t t0 = esp_timer_get_time();
    play("P6a_SHORT", CLIP_SHORT, CLIP_SHORT_LEN);
    play("P6b_SHORT", CLIP_SHORT, CLIP_SHORT_LEN);
    int64_t total = esp_timer_get_time() - t0;
    unsigned expected_total =
        2 * (unsigned)((uint64_t)(CLIP_SHORT_LEN / 2) * 1000000ULL /
                       (uint64_t)SAMPLE_RATE);
    double drift = 100.0 * ((double)total - (double)expected_total) /
                   (double)expected_total;
    DLOG("[SPKDIAG] P6_TOTAL: took_us=%lld expected=%u drift=%.2f%%\n",
           (long long)total, expected_total, drift);

    DLOG("[SPKDIAG] ================ SWEEP DONE ================\n");
    DLOG("[SPKDIAG] Idle. OTA server still listening — push updates anytime.\n");
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
