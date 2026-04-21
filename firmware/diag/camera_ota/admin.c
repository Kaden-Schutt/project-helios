/*
 * Admin endpoints: inspect OTA slot state, list SD firmware library,
 * manually flip boot partition or load a specific signed image from SD.
 *
 *   GET  /admin/partitions
 *   GET  /admin/firmwares
 *   POST /admin/boot-from-sd?file=<basename>[&force=1]
 *   POST /admin/boot-from-sd?tag=<debug|experimental|prod>[&force=1]
 *   POST /admin/boot?slot=ota_0|ota_1[&force=1]
 *   POST /admin/pin-recovery?file=<basename>
 */

#include "admin.h"
#include "sd_card.h"
#include "recovery.h"
#include "diag_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "admin";
#define FW_DIR "/sd/firmwares"

#ifndef FW_TAG
#define FW_TAG "debug"
#endif

/* --- helpers --- */

static const char *state_str(esp_ota_img_states_t s)
{
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "unknown";
    }
}

/* Parse url-encoded query like "file=foo.signed.bin&tag=prod" into key value. */
static bool get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_cap)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return false;
    char q[256];
    if (qlen >= sizeof(q)) return false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, out_cap) != ESP_OK) return false;
    return true;
}

/* Parse "<name>-<tag>-<version>.signed.bin" into its three parts.
 * Returns true on success. */
static bool parse_fw_filename(const char *fname,
                              char *out_name, size_t name_cap,
                              char *out_tag,  size_t tag_cap,
                              char *out_ver,  size_t ver_cap)
{
    /* must end with ".signed.bin" */
    size_t len = strlen(fname);
    const char *suffix = ".signed.bin";
    size_t slen = strlen(suffix);
    if (len <= slen) return false;
    if (strcmp(fname + len - slen, suffix) != 0) return false;

    char stem[128];
    size_t stem_len = len - slen;
    if (stem_len >= sizeof(stem)) return false;
    memcpy(stem, fname, stem_len);
    stem[stem_len] = 0;

    /* Split by first two dashes. name | tag | rest(=version). */
    const char *d1 = strchr(stem, '-');
    if (!d1) return false;
    const char *d2 = strchr(d1 + 1, '-');
    if (!d2) return false;

    size_t nlen = d1 - stem;
    size_t tlen = d2 - d1 - 1;
    size_t vlen = stem + stem_len - d2 - 1;
    if (nlen >= name_cap || tlen >= tag_cap || vlen >= ver_cap) return false;
    memcpy(out_name, stem, nlen); out_name[nlen] = 0;
    memcpy(out_tag, d1 + 1, tlen); out_tag[tlen] = 0;
    memcpy(out_ver, d2 + 1, vlen); out_ver[vlen] = 0;
    return true;
}

/* --- handlers --- */

static esp_err_t partitions_get(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    char body[2048];
    int off = snprintf(body, sizeof(body),
        "{\n  \"running\": \"%s\",\n  \"boot\": \"%s\",\n  \"fw_tag\": \"%s\",\n  \"slots\": [",
        running ? running->label : "?", boot ? boot->label : "?", FW_TAG);

    const esp_partition_t *p = NULL;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    while (it) {
        p = esp_partition_get(it);
        if (p->type == ESP_PARTITION_TYPE_APP && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
            && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
            esp_ota_get_state_partition(p, &state);
            esp_app_desc_t desc = {0};
            bool have_desc = (esp_ota_get_partition_description(p, &desc) == ESP_OK);
            off += snprintf(body + off, sizeof(body) - off,
                "%s\n    {\"label\": \"%s\", \"state\": \"%s\", "
                "\"size\": %lu, \"addr\": \"0x%lx\"",
                first ? "" : ",", p->label, state_str(state),
                (unsigned long)p->size, (unsigned long)p->address);
            if (have_desc) {
                off += snprintf(body + off, sizeof(body) - off,
                    ", \"app\": \"%s\", \"version\": \"%s\", "
                    "\"compile_time\": \"%s %s\"",
                    desc.project_name, desc.version, desc.date, desc.time);
            }
            off += snprintf(body + off, sizeof(body) - off, "}");
            first = false;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    off += snprintf(body + off, sizeof(body) - off, "\n  ]\n}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, off);
}

static esp_err_t firmwares_get(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no SD");
    }
    DIR *d = opendir(FW_DIR);
    if (!d) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no " FW_DIR);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[\n", 2);
    struct dirent *e;
    bool first = true;
    char line[512];
    while ((e = readdir(d)) != NULL) {
        char name[64], tag[16], ver[64];
        if (!parse_fw_filename(e->d_name, name, sizeof(name),
                               tag, sizeof(tag), ver, sizeof(ver))) continue;
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", FW_DIR, e->d_name);
        struct stat st;
        size_t size = (stat(full, &st) == 0) ? (size_t)st.st_size : 0;
        int n = snprintf(line, sizeof(line),
            "%s  {\"file\": \"%s\", \"name\": \"%s\", \"tag\": \"%s\", "
            "\"version\": \"%s\", \"size\": %u}",
            first ? "" : ",\n", e->d_name, name, tag, ver, (unsigned)size);
        httpd_resp_send_chunk(req, line, n);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "\n]\n", 3);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Pick newest (mtime) firmware file in /sd/firmwares matching the given tag.
 * Returns true if found, filling out_basename. */
static bool pick_by_tag(const char *target_tag, char *out_basename, size_t cap)
{
    if (!sd_card_is_mounted()) return false;
    DIR *d = opendir(FW_DIR);
    if (!d) return false;
    struct dirent *e;
    time_t best_t = 0;
    bool found = false;
    char best[128] = {0};
    while ((e = readdir(d)) != NULL) {
        char name[64], tag[16], ver[64];
        if (!parse_fw_filename(e->d_name, name, sizeof(name),
                               tag, sizeof(tag), ver, sizeof(ver))) continue;
        if (strcmp(tag, target_tag) != 0) continue;
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", FW_DIR, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!found || st.st_mtime > best_t) {
            best_t = st.st_mtime;
            strncpy(best, e->d_name, sizeof(best) - 1);
            found = true;
        }
    }
    closedir(d);
    if (!found) return false;
    strncpy(out_basename, best, cap - 1);
    out_basename[cap - 1] = 0;
    return true;
}

static esp_err_t boot_from_sd_post(httpd_req_t *req)
{
    char fileparam[128] = {0};
    char tag[16] = {0};
    char force[8] = {0};
    get_query_param(req, "file",  fileparam, sizeof(fileparam));
    get_query_param(req, "tag",   tag,       sizeof(tag));
    get_query_param(req, "force", force,     sizeof(force));
    bool force_valid = (force[0] == '1' || force[0] == 't');

    char basename[128] = {0};
    if (fileparam[0]) {
        strncpy(basename, fileparam, sizeof(basename) - 1);
    } else if (tag[0]) {
        if (!pick_by_tag(tag, basename, sizeof(basename))) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                       "no firmware with that tag on SD");
        }
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "need file=<basename> or tag=<tag>");
    }

    char full[256];
    snprintf(full, sizeof(full), "%s/%s", FW_DIR, basename);
    if (!sd_card_exists(full)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not on SD");
    }

    const esp_partition_t *slot = NULL;
    esp_err_t err = recovery_apply_signed_file(full, &slot);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "apply failed 0x%x", err);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "set_boot failed 0x%x", err);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    if (force_valid) {
        /* Pre-validate: skip pending-verify → rollback would otherwise kick in
         * if the target fw doesn't call mark_valid itself. Dangerous, use only
         * if you're sure the target is good. */
        esp_ota_mark_app_valid_cancel_rollback();
    }
    char ok[256];
    int n = snprintf(ok, sizeof(ok),
        "OK — flashed %s into %s, rebooting in 1s%s\n",
        basename, slot->label, force_valid ? " (force-valid)" : "");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ok, n);
    sd_card_unmount();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t boot_slot_post(httpd_req_t *req)
{
    char slot_param[16] = {0};
    char force[8] = {0};
    if (!get_query_param(req, "slot", slot_param, sizeof(slot_param))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need slot=");
    }
    get_query_param(req, "force", force, sizeof(force));
    bool force_valid = (force[0] == '1' || force[0] == 't');

    const esp_partition_t *target = NULL;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (strcmp(p->label, slot_param) == 0) { target = p; break; }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    if (!target) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such slot");
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(target, &state);
    if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "target slot is invalid/aborted");
    }
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "set_boot failed 0x%x", err);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    if (force_valid) esp_ota_mark_app_valid_cancel_rollback();
    char ok[128];
    int n = snprintf(ok, sizeof(ok), "OK — boot set to %s, reboot in 1s%s\n",
                     target->label, force_valid ? " (force-valid)" : "");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ok, n);
    sd_card_unmount();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* Accept a signed firmware body and write it to /sd/firmwares/<name>.
 * Useful for populating the SD library over-the-air. Signature is NOT
 * verified on upload — it's verified when you boot it via /admin/boot-from-sd.
 * That's intentional: this endpoint is just file transfer; boot-time
 * verify is the gate that matters. */
static esp_err_t upload_fw_post(httpd_req_t *req)
{
    char name[128] = {0};
    if (!get_query_param(req, "name", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need name=");
    }
    /* Basic sanity: must end with .signed.bin and have no path separators. */
    if (strchr(name, '/') || strchr(name, '\\')) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no path seps");
    }
    size_t nlen = strlen(name);
    const char *suffix = ".signed.bin";
    size_t slen = strlen(suffix);
    if (nlen <= slen || strcmp(name + nlen - slen, suffix) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "must end with .signed.bin");
    }
    if (!sd_card_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD");
    }
    mkdir(FW_DIR, 0777);   /* idempotent; ignores exist */

    char dst[320];
    snprintf(dst, sizeof(dst), "%s/%s.tmp", FW_DIR, name);
    char final[320];
    snprintf(final, sizeof(final), "%s/%s", FW_DIR, name);

    FILE *f = sd_card_open_write(dst);
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");

    char buf[2048];
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { fclose(f); return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv"); }
        if ((int)fwrite(buf, 1, got, f) != got) {
            fclose(f);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        total += got;
        remaining -= got;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!sd_card_rename(dst, final)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    char ok[256];
    int nn = snprintf(ok, sizeof(ok),
        "OK — wrote %s (%d bytes). Boot via:\n"
        "  curl -X POST 'http://<host>/admin/boot-from-sd?file=%s'\n",
        final, total, name);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ok, nn);
}

static esp_err_t pin_recovery_post(httpd_req_t *req)
{
    char fileparam[128] = {0};
    if (!get_query_param(req, "file", fileparam, sizeof(fileparam))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need file=");
    }
    if (!sd_card_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD");
    }
    char src[256];
    snprintf(src, sizeof(src), "%s/%s", FW_DIR, fileparam);
    if (!sd_card_exists(src)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not on SD");
    }
    /* Copy src → /sd/recovery.signed.bin */
    FILE *in = sd_card_open_read(src);
    FILE *out = sd_card_open_write("/sd/recovery.signed.bin.new");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    char buf[2048];
    size_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        total += n;
    }
    fclose(in);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    if (!sd_card_rename("/sd/recovery.signed.bin.new", "/sd/recovery.signed.bin")) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    char ok[192];
    int nn = snprintf(ok, sizeof(ok),
        "OK — pinned %s (%u bytes) as /sd/recovery.signed.bin\n",
        fileparam, (unsigned)total);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ok, nn);
}

/* --- registration --- */

void admin_register(httpd_handle_t server)
{
    static const httpd_uri_t h[] = {
        { .uri = "/admin/partitions",   .method = HTTP_GET,  .handler = partitions_get   },
        { .uri = "/admin/firmwares",    .method = HTTP_GET,  .handler = firmwares_get    },
        { .uri = "/admin/boot-from-sd", .method = HTTP_POST, .handler = boot_from_sd_post },
        { .uri = "/admin/boot",         .method = HTTP_POST, .handler = boot_slot_post    },
        { .uri = "/admin/pin-recovery", .method = HTTP_POST, .handler = pin_recovery_post },
        { .uri = "/admin/upload-fw",    .method = HTTP_POST, .handler = upload_fw_post    },
    };
    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
        httpd_register_uri_handler(server, &h[i]);
    }
    ESP_LOGI(TAG, "admin endpoints registered (%u)", (unsigned)(sizeof(h)/sizeof(h[0])));
}
