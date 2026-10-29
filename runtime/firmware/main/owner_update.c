/* The owner's Wi-Fi update service; see owner_update.h and UPDATES.md.
 *
 * Same contract as the owner-modified stock firmware
 * (desk/OTA-UPDATES.md, shared/owner_update*.c):
 *  - every route needs "Authorization: Bearer <device key, hex>" and is refused
 *    to browsers (any Origin or Sec-Fetch-* header);
 *  - uploads go only to the other application slot, through the same
 *    validator (owner_ota.c: one FWOTA1 descriptor with this device's key,
 *    transfer SHA-256, ESP image checks), then a flash read-back hash and the
 *    SDK's image verification; only then is the next boot selected;
 *  - writes need If-Match with the running image's hash.
 * Owned rules on top:
 *  - a confirmation keeps a pending image only in a build made keepable
 *    (CONFIG_FW_KEEPABLE) and only when it is healthy (online, panel running);
 *    test builds refuse, so they always roll back;
 *  - /switch boots the other slot (normally stock) once this image is kept;
 *    the other image is verified by the SDK before it is selected.
 * Never logs the key. */
#include "owner_update.h"
#include "wall_log.h"
#include "crash_guard.h"
#include "board_check.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "memory_probe.h"
#include "owner_descriptor.h"
#include "owner_ota.h"
#include "wifi_link.h"

#define SLOT_SIZE 0x2c0000u
#ifdef CONFIG_FW_KEEPABLE
#define KEEPABLE 1
#else
#define KEEPABLE 0
#endif

static httpd_handle_t s_server;
static const esp_partition_t *s_running, *s_target;
static uint8_t s_running_hash[32];
static bool s_ready, s_pending, s_confirmed, s_display_ok;
static volatile bool s_busy;
static esp_ota_handle_t s_ota;
static bool s_ota_open;
static uint32_t s_written, s_length;
static OwnerOtaContext s_upload;

bool fw_owner_update_confirmed(void){ return s_confirmed; }

/* ------------------------------------------------------------ images */

static bool app_slot(const esp_partition_t *p){
    return p && p->type == ESP_PARTITION_TYPE_APP && p->size == SLOT_SIZE &&
           (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

static const char *slot_name(const esp_partition_t *p){
    return p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "app0" : "app1";
}

static uint32_t get32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Length of the image in a slot, appended SHA-256 included (as stock). */
static bool image_length(const esp_partition_t *p, uint32_t *length){
    uint8_t header[24], row[8];
    uint32_t at = 24;
    if (!app_slot(p) || esp_partition_read(p, 0, header, sizeof header) != ESP_OK || header[0] != 0xe9 ||
        !header[1] || header[1] > 16 || header[23] != 1)
        return false;
    for (unsigned i = 0; i < header[1]; ++i) {
        uint32_t n;
        if (at > p->size - 8 || esp_partition_read(p, at, row, sizeof row) != ESP_OK) return false;
        n = get32(row + 4);
        at += 8;
        if (!n || n > p->size - at) return false;
        at += n;
    }
    at = at / 16u * 16u + 48u;
    if (at > p->size) return false;
    *length = at;
    return true;
}

/* SHA-256 of the whole image file, as the desk computes it. */
static bool hash_slot(const esp_partition_t *p, uint32_t length, uint8_t out[32]){
    static uint8_t buf[1024];
    OwnerSha256 h;
    uint32_t at = 0;
    owner_sha256_init(&h);
    while (at < length) {
        uint32_t n = length - at < sizeof buf ? length - at : sizeof buf;
        if (esp_partition_read(p, at, buf, n) != ESP_OK) return false;
        owner_sha256_update(&h, buf, n);
        at += n;
    }
    owner_sha256_final(&h, out);
    return true;
}

static bool hash_image(const esp_partition_t *p, uint8_t out[32]){
    uint32_t n;
    return image_length(p, &n) && hash_slot(p, n, out);
}

/* The running image is the boot choice, nothing else is selected. */
static bool current(void){
    const esp_partition_t *boot = esp_ota_get_boot_partition(), *run = esp_ota_get_running_partition();
    return s_ready && app_slot(boot) && run == s_running && boot == s_running;
}

/* ------------------------------------------------------------ http */

static void hex32(char out[65], const uint8_t in[32]){
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[64] = 0;
}

static bool parse_hex32(const char *s, size_t n, uint8_t out[32]){
    if (n != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        unsigned c = (unsigned char)s[i];
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else return false;
        if (!(i & 1)) out[i / 2] = (uint8_t)(c << 4);
        else out[i / 2] |= (uint8_t)c;
    }
    return true;
}

static esp_err_t reply(httpd_req_t *req, int status, const char *body){
    char line[40];
    const char *why = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" :
                      status == 403 ? "Forbidden" : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" :
                      status == 415 ? "Unsupported Media Type" : status == 422 ? "Unprocessable Content" :
                      status == 428 ? "Precondition Required" : "Request Rejected";
    snprintf(line, sizeof line, "%d %s", status, why);
    httpd_resp_set_status(req, line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, body ? body : "{\"error\":\"Firmware request rejected\"}");
}

static esp_err_t error(httpd_req_t *req, int status, const char *why){
    char body[96], extra[128];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", why);
    static unsigned refused;
    snprintf(extra, sizeof extra, "\"path\":\"%.24s\",\"status\":%d,\"why\":\"%.40s\",\"n\":%u", req->uri, status, why,
             ++refused);
    if (refused <= 3 || refused % 100 == 0) fw_mem_report("update_refused", extra);   /* the 7 KiB log holds hours */
    return reply(req, status, body);
}

static bool has_header(httpd_req_t *req, const char *name){ return httpd_req_get_hdr_value_len(req, name) > 0; }

/* 0 when the request may go on, else the HTTP status to refuse it with. */
static int gate(httpd_req_t *req){
    static const char hex[] = "0123456789abcdef";
    char auth[80];
    uint8_t key[32], any = 0, diff = 0;
    fw_owner_descriptor_key(key);
    for (int i = 0; i < 32; ++i) any |= key[i];
    if (!any) return 503;                                      /* unpackaged build: no key */
    if (httpd_req_get_hdr_value_len(req, "Authorization") != 71 ||
        httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof auth) != ESP_OK)
        diff = 1;
    else {
        static const char prefix[] = "bearer ";
        for (int i = 0; i < 7; ++i) diff |= (uint8_t)((auth[i] | 0x20) ^ prefix[i]);
        for (int i = 0; i < 32; ++i) {
            diff |= (uint8_t)(auth[7 + 2 * i] ^ hex[key[i] >> 4]);
            diff |= (uint8_t)(auth[8 + 2 * i] ^ hex[key[i] & 15]);
        }
    }
    memset(key, 0, sizeof key);
    memset(auth, 0, sizeof auth);
    if (diff) return 401;
    if (has_header(req, "Origin") || has_header(req, "Sec-Fetch-Site") || has_header(req, "Sec-Fetch-Mode") ||
        has_header(req, "Sec-Fetch-Dest") || has_header(req, "Sec-Fetch-User"))
        return 403;                                            /* never from a browser page */
    return 0;
}

/* If-Match: "<running hash>" */
static int if_match(httpd_req_t *req){
    char v[70];
    uint8_t want[32];
    if (httpd_req_get_hdr_value_len(req, "If-Match") != 66 ||
        httpd_req_get_hdr_value_str(req, "If-Match", v, sizeof v) != ESP_OK)
        return 428;
    if (v[0] != '"' || v[65] != '"' || !parse_hex32(v + 1, 64, want)) return 400;
    return memcmp(want, s_running_hash, 32) || !current() ? 409 : 0;
}

/* What the slot not running holds, by its app description: "vendor" (the
 * stock firmware kept next to FreeBirdWall), "owned", "empty" or "other". A
 * Wi-Fi update replaces it (UPDATES.md "The vendor firmware in the spare
 * slot"), so the desk says when that was the stock firmware. */
static const char *spare_kind(void){
    esp_app_desc_t d;
    const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
    if (!app_slot(spare) || spare == s_running || esp_ota_get_partition_description(spare, &d) != ESP_OK) return "empty";
    if (!strcmp(d.project_name, "arduino-lib-builder")) return "vendor";
    if (!strcmp(d.project_name, "freebirdwall") || !strcmp(d.project_name, "flightwall-owned")) return "owned";
    return "other";
}

static void status_json(char *out, size_t cap){
    char h[65];
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    hex32(h, s_running_hash);
    snprintf(out, cap,
             "{\"api_version\":1,\"schema_version\":1,\"ota_supported\":true,\"running_slot\":\"%s\","
             "\"running_sha256\":\"%s\",\"partition_size\":%u,\"boot_slot\":\"%s\",\"rollback_supported\":true,"
             "\"update_busy\":%s,\"pending_confirmation\":%s,\"firmware\":\"owned\",\"keepable\":%s,\"safe_mode\":%s,\"board_ok\":%s,"
             "\"spare_slot\":\"%s\"}",
             slot_name(s_running), h, (unsigned)s_running->size, app_slot(boot) ? slot_name(boot) : slot_name(s_running),
             s_busy ? "true" : "false", s_pending ? "true" : "false", KEEPABLE ? "true" : "false",
             fw_safe_mode() ? "true" : "false", fw_board()->ok ? "true" : "false", s_busy ? "busy" : spare_kind());
}

static esp_err_t get_status(httpd_req_t *req){
    char body[448];
    int g = gate(req);
    if (g) return error(req, g, "not allowed");
    if (!s_ready) return error(req, 503, "not ready");
    status_json(body, sizeof body);
    return reply(req, 200, body);
}

/* The kept status lines (wall_log.h), oldest first, as plain text. */
static esp_err_t get_log(httpd_req_t *req){
    char chunk[512];
    size_t at = 0, n;
    int g = gate(req);
    if (g) return error(req, g, "not allowed");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while ((n = fw_log_read(at, chunk, sizeof chunk)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) return ESP_FAIL;
        at += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void restart_cb(void *arg){
    (void)arg;
    fw_state("{\"event\":\"update_restart\"}");
    esp_restart();
}

static void restart_soon(void){
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "fw_upd_restart"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK) { esp_restart(); return; }
    esp_timer_start_once(t, 300000);
}

/* ------------------------------------------------------------ upload */

static int op_begin(void *u, uint32_t length){
    (void)u;
    if (!current() || s_pending || !app_slot(s_target) || s_target == s_running || length > s_target->size) return -1;
    s_written = 0;
    s_length = length;
    if (esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) != ESP_OK) return -1;
    s_ota_open = true;
    return 0;
}

static int op_write(void *u, const unsigned char *p, size_t n){
    (void)u;
    if (n > s_length - s_written || esp_ota_write(s_ota, p, n) != ESP_OK) return -1;
    s_written += (uint32_t)n;
    return 0;
}

static int op_end(void *u){
    uint8_t got[32];
    (void)u;
    s_ota_open = false;
    if (s_written != s_length || esp_ota_end(s_ota) != ESP_OK) return -1;        /* SDK image verification */
    return !hash_slot(s_target, s_length, got) || memcmp(got, s_upload.expected_hash, 32) ? -1 : 0;   /* read-back */
}

static void op_abort(void *u){
    (void)u;
    if (s_ota_open) esp_ota_abort(s_ota);
    s_ota_open = false;
}

static int op_select(void *u){
    (void)u;
    return current() && !s_pending && esp_ota_set_boot_partition(s_target) == ESP_OK ? 0 : -1;
}

static esp_err_t put_firmware(httpd_req_t *req){
    static uint8_t buf[2048];
    static const OwnerOtaOps ops = {NULL, op_begin, op_write, op_end, op_abort, op_select};
    char type[40], sha_hex[70], body[256], h[65];
    uint8_t sha[32], key[32];
    int g = gate(req), rc, idle = 0;
    size_t left;
    if (g) return error(req, g, "not allowed");
    if (!s_ready) return error(req, 503, "not ready");
    if ((g = if_match(req))) return error(req, g, g == 409 ? "firmware changed" : "If-Match required");
    if (s_pending) return error(req, 409, "this image is not confirmed yet");
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof type) != ESP_OK ||
        strcmp(type, "application/octet-stream") != 0)
        return error(req, 415, "binary body required");
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", sha_hex, sizeof sha_hex) != ESP_OK ||
        !parse_hex32(sha_hex, 64, sha))
        return error(req, 400, "X-Firmware-SHA256 required");
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!app_slot(s_target) || s_target == s_running || req->content_len < OO_MIN_IMAGE ||
        req->content_len > s_target->size)
        return error(req, 413, "image size");
    if (s_busy) return error(req, 409, "busy");
    s_busy = true;
    fw_mem_report("update_upload_start", NULL);
    fw_owner_descriptor_key(key);
    rc = owner_ota_start(&s_upload, &ops, (uint32_t)req->content_len, sha, key, s_target->size);
    memset(key, 0, sizeof key);
    for (left = req->content_len; rc == OO_OK && left;) {
        int r = httpd_req_recv(req, (char *)buf, left < sizeof buf ? left : sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle > 6) { owner_ota_cancel(&s_upload); rc = OO_IO; }
            continue;
        }
        if (r <= 0) { owner_ota_cancel(&s_upload); rc = OO_IO; break; }
        idle = 0;
        rc = owner_ota_feed(&s_upload, buf, (size_t)r);
        left -= (size_t)r;
    }
    if (rc == OO_OK) rc = owner_ota_finish(&s_upload);
    s_busy = false;
    {
        char extra[48];
        snprintf(extra, sizeof extra, "\"rc\":%d,\"bytes\":%u", rc, (unsigned)s_written);
        fw_mem_report("update_upload_end", extra);
    }
    if (rc != OO_OK) return error(req, 422, rc == OO_KEY ? "not for this device" : "image rejected");
    hex32(h, sha);
    snprintf(body, sizeof body,
             "{\"api_version\":1,\"schema_version\":1,\"accepted\":true,\"sha256\":\"%s\",\"bytes\":%u,"
             "\"target_slot\":\"%s\",\"rebooting\":true}",
             h, (unsigned)req->content_len, slot_name(s_target));
    reply(req, 200, body);
    restart_soon();
    return ESP_OK;
}

/* The vendor firmware, by its app description: an ESP32-S3 image whose
 * project is the vendor's (as the installer's slotKind reads it). */
static bool vendor_image(const uint8_t *p, size_t n){
    static const char project[] = "arduino-lib-builder";
    return n >= 112 && p[0] == 0xe9 && (p[12] | p[13] << 8) == 9 && get32(p + 32) == 0xabcd5432 &&
           !memcmp(p + 80, project, sizeof project);
}

static bool recv_full(httpd_req_t *req, uint8_t *p, size_t n){
    int idle = 0;
    for (size_t got = 0; got < n;) {
        int r = httpd_req_recv(req, (char *)p + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++idle <= 6) continue;
        if (r <= 0) return false;
        idle = 0;
        got += (size_t)r;
    }
    return true;
}

/* PUT /v1/firmware/spare: the vendor firmware, unchanged, into the slot that
 * is not running, as an install leaves the wall (UPDATES.md "The vendor
 * firmware in the spare slot"). What starts does not change: the installer's
 * "Start the vendor firmware again" starts it over USB. Only the vendor's
 * image (vendor_image, before anything is written); the transfer hash, the
 * SDK's image verification and a read-back as for an upload. */
static esp_err_t put_spare(httpd_req_t *req){
    static uint8_t buf[2048];
    char type[40], sha_hex[70], body[256], h[65];
    uint8_t sha[32], got[32];
    OwnerSha256 hs;
    size_t left, n;
    bool ok;
    int g = gate(req);
    if (g) return error(req, g, "not allowed");
    if (!s_ready) return error(req, 503, "not ready");
    if ((g = if_match(req))) return error(req, g, g == 409 ? "firmware changed" : "If-Match required");
    if (s_pending) return error(req, 409, "this image is not confirmed yet");
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof type) != ESP_OK ||
        strcmp(type, "application/octet-stream") != 0)
        return error(req, 415, "binary body required");
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", sha_hex, sizeof sha_hex) != ESP_OK ||
        !parse_hex32(sha_hex, 64, sha))
        return error(req, 400, "X-Firmware-SHA256 required");
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!app_slot(s_target) || s_target == s_running || req->content_len < sizeof buf ||
        req->content_len > s_target->size)
        return error(req, 413, "image size");
    if (s_busy) return error(req, 409, "busy");
    if (!recv_full(req, buf, sizeof buf)) return error(req, 400, "upload cut off");
    if (!vendor_image(buf, sizeof buf)) return error(req, 422, "not the vendor firmware");
    s_busy = true;
    fw_mem_report("spare_upload_start", NULL);
    owner_sha256_init(&hs);
    s_written = 0;
    s_length = (uint32_t)req->content_len;
    ok = current() && esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) == ESP_OK;
    s_ota_open = ok;
    for (n = sizeof buf, left = req->content_len; ok;) {
        owner_sha256_update(&hs, buf, n);
        ok = esp_ota_write(s_ota, buf, n) == ESP_OK;
        s_written += (uint32_t)n;
        left -= n;
        if (!ok || !left) break;
        int r = 0;
        for (int idle = 0; (r = httpd_req_recv(req, (char *)buf, left < sizeof buf ? left : sizeof buf)) ==
                           HTTPD_SOCK_ERR_TIMEOUT && ++idle <= 6;)
            ;
        ok = r > 0;
        n = ok ? (size_t)r : 0;
    }
    if (ok) {
        owner_sha256_final(&hs, got);
        ok = !memcmp(got, sha, 32);
    }
    if (ok) {
        s_ota_open = false;
        ok = esp_ota_end(s_ota) == ESP_OK &&                                  /* SDK image verification */
             hash_slot(s_target, s_length, got) && !memcmp(got, sha, 32);    /* read-back */
    }
    if (s_ota_open) esp_ota_abort(s_ota);
    s_ota_open = false;
    s_busy = false;
    {
        char extra[48];
        snprintf(extra, sizeof extra, "\"ok\":%s,\"bytes\":%u", ok ? "true" : "false", (unsigned)s_written);
        fw_mem_report("spare_upload_end", extra);
    }
    if (!ok) return error(req, 422, "image rejected");
    hex32(h, sha);
    snprintf(body, sizeof body,
             "{\"api_version\":1,\"schema_version\":1,\"stored\":true,\"sha256\":\"%s\",\"bytes\":%u,\"slot\":\"%s\"}",
             h, (unsigned)req->content_len, slot_name(s_target));
    return reply(req, 200, body);
}

/* ------------------------------------------------------------ confirm / switch */

static esp_err_t post_confirm(httpd_req_t *req){
    char body[448];
    int g = gate(req);
    if (g) return error(req, g, "not allowed");
    if (!s_ready) return error(req, 503, "not ready");
    if (req->content_len) return error(req, 400, "no body");
    if ((g = if_match(req))) return error(req, g, g == 409 ? "firmware changed" : "If-Match required");
    if (s_pending) {
        if (!KEEPABLE) return error(req, 409, "test build: rolls back, never kept");
        if (!s_display_ok || !fw_wifi_link_wait_online(0)) return error(req, 503, "not healthy yet");
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return error(req, 503, "could not confirm");
        s_pending = false;
        s_confirmed = true;
        fw_mem_report("update_confirmed", NULL);
    }
    status_json(body, sizeof body);
    return reply(req, 200, body);
}

static esp_err_t post_switch(httpd_req_t *req){
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;
    esp_app_desc_t desc;
    uint8_t hash[32];
    char body[256], h[65];
    int g = gate(req);
    if (g) return error(req, g, "not allowed");
    if (!s_ready) return error(req, 503, "not ready");
    if (req->content_len) return error(req, 400, "no body");
    if ((g = if_match(req))) return error(req, g, g == 409 ? "firmware changed" : "If-Match required");
    if (s_pending) return error(req, 409, "this image is not confirmed: it rolls back by itself");
    if (s_busy) return error(req, 409, "busy");
    if (!app_slot(other) || other == s_running || esp_ota_get_partition_description(other, &desc) != ESP_OK)
        return error(req, 409, "no image in the other slot");
    if (esp_ota_get_state_partition(other, &state) == ESP_OK &&
        (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED))
        return error(req, 409, "the other image failed before");
    if (!hash_image(other, hash)) return error(req, 409, "the other image is unreadable");
    if (esp_ota_set_boot_partition(other) != ESP_OK) return error(req, 422, "the other image did not verify");
    hex32(h, hash);
    snprintf(body, sizeof body,
             "{\"api_version\":1,\"schema_version\":1,\"switching\":true,\"target_slot\":\"%s\","
             "\"target_sha256\":\"%s\",\"rebooting\":true}",
             slot_name(other), h);
    fw_mem_report("update_switch", NULL);
    reply(req, 200, body);
    restart_soon();
    return ESP_OK;
}

bool fw_owner_update_start(bool display_ok){
    esp_ota_img_states_t state;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    static const httpd_uri_t routes[] = {
        {.uri = "/v1/firmware", .method = HTTP_GET, .handler = get_status},
        {.uri = "/v1/firmware", .method = HTTP_PUT, .handler = put_firmware},
        {.uri = "/v1/firmware/confirm", .method = HTTP_POST, .handler = post_confirm},
        {.uri = "/v1/firmware/switch", .method = HTTP_POST, .handler = post_switch},
        {.uri = "/v1/firmware/spare", .method = HTTP_PUT, .handler = put_spare},
        {.uri = "/v1/log", .method = HTTP_GET, .handler = get_log},
    };
    char extra[96];
    s_display_ok = display_ok;
    s_running = esp_ota_get_running_partition();
    s_pending = esp_ota_get_state_partition(s_running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    s_ready = app_slot(s_running) && hash_image(s_running, s_running_hash);
    snprintf(extra, sizeof extra, "\"ready\":%s,\"pending\":%s,\"keepable\":%s,\"keyed\":%s", s_ready ? "true" : "false",
             s_pending ? "true" : "false", KEEPABLE ? "true" : "false",
             fw_owner_descriptor_keyed() ? "true" : "false");
    fw_mem_report("update_service", extra);
    c.server_port = FW_UPDATE_PORT;
    c.ctrl_port = 32773;
    c.stack_size = 6144;
    c.max_open_sockets = 2;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    if (httpd_start(&s_server, &c) != ESP_OK) {
        s_server = NULL;
        return false;
    }
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) httpd_register_uri_handler(s_server, &routes[i]);
    return true;
}
