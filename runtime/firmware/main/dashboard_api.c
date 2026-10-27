/* The owned dashboard's settings API (SETTINGS.md "API"). Signing in pairs a
 * browser with a code shown on the wall; every settings route then needs
 * "Authorization: Bearer <key>" (the paired browser's key, or the device key
 * the desk uses). Never logs keys, codes or settings values. */
#include "dashboard_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "aliases.h"
#include "feed.h"
#include "fw_pair.h"
#include "fw_settings.h"
#include "memory_probe.h"
#include "owner_alias.h"
#include "owner_descriptor.h"
#include "settings_store.h"
#include "stats.h"
#include "big_buffer.h"
#include "wall_log.h"
#include "sync_store.h"
#include "app_settings.h"
#include "aero_key.h"
#include "community.h"
#include "fw_assets.h"
#include "fw_logo.h"

#define BODY_MAX 8192
#define PAIR_START_GAP_MS 5000

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body){
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t error(httpd_req_t *req, const char *status, const char *why){
    char body[160];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", why);
    return reply(req, status, body);
}

/* Reads the body (at most BODY_MAX). Returns NULL on failure (reply sent). */
static char *body(httpd_req_t *req){
    char *b;
    int got = 0;
    if (req->content_len > BODY_MAX) { error(req, "413 Payload Too Large", "too large"); return NULL; }
    b = malloc(req->content_len + 1);
    if (!b) { error(req, "500 Internal Server Error", "out of memory"); return NULL; }
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(b); return NULL; }
        got += r;
    }
    b[got] = 0;
    return b;
}

/* The browser's key, or the device key (desk). */
static bool signed_in(httpd_req_t *req){
    static const char hex[] = "0123456789abcdef";
    char auth[80];
    bool ok;
    if (httpd_req_get_hdr_value_len(req, "Authorization") != 71 ||
        httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof auth) != ESP_OK ||
        strncmp(auth, "Bearer ", 7))
        return false;
    {
        FwPair *p = fw_store_pair_lock();
        ok = p && fw_pair_check(p, auth + 7);
        fw_store_pair_unlock(false);
    }
    if (!ok) {
        uint8_t key[32], any = 0, diff = 0;
        fw_owner_descriptor_key(key);
        for (int i = 0; i < 32; ++i) {
            any |= key[i];
            diff |= (uint8_t)(auth[7 + 2 * i] ^ hex[key[i] >> 4]);
            diff |= (uint8_t)(auth[8 + 2 * i] ^ hex[key[i] & 15]);
        }
        ok = any && !diff;
        memset(key, 0, sizeof key);
    }
    memset(auth, 0, sizeof auth);
    return ok;
}

static bool gate(httpd_req_t *req){
    FwPair *p;
    if (signed_in(req)) return true;
    /* Just after start the sign-ins may not be loaded yet: "try again", not
     * "signed out" (a 401 makes the page forget its key). */
    if (!(p = fw_store_pair_lock())) { error(req, "503 Service Unavailable", "starting up; try again"); return false; }
    fw_store_pair_unlock(false);
    error(req, "401 Unauthorized", "sign in on the dashboard first");
    return false;
}

static bool if_match(httpd_req_t *req, uint32_t *rev){
    char v[24], *end;
    unsigned long n;
    if (httpd_req_get_hdr_value_str(req, "If-Match", v, sizeof v) != ESP_OK) return false;
    n = strtoul(v[0] == '"' ? v + 1 : v, &end, 10);
    *rev = (uint32_t)n;
    return end != v;
}

/* ------------------------------------------------------------ signing in */

static esp_err_t session_get(httpd_req_t *req){
    char b[96];
    FwPair *p = fw_store_pair_lock();
    int left = 0;
    bool showing = p && fw_pair_code(p, now_ms(), &left) != NULL;
    fw_store_pair_unlock(false);
    snprintf(b, sizeof b, "{\"signed_in\":%s,\"code_showing\":%s,\"seconds_left\":%d}", signed_in(req) ? "true" : "false",
             showing ? "true" : "false", showing ? left : 0);
    return reply(req, "200 OK", b);
}

static esp_err_t pair_start(httpd_req_t *req){
    static uint32_t last;
    uint32_t now = now_ms();
    FwPair *p;
    if (last && now - last < PAIR_START_GAP_MS) return error(req, "429 Too Many Requests", "wait a few seconds");
    if (!(p = fw_store_pair_lock())) return error(req, "503 Service Unavailable", "starting up; try again");
    last = now;
    fw_pair_start(p, now, esp_random());
    fw_store_pair_unlock(false);
    fw_mem_report("pair_code_shown", NULL);
    return reply(req, "200 OK", "{\"seconds\":120}");
}

static esp_err_t pair_finish(httpd_req_t *req){
    char *b = body(req), key_hex[65], out[96];
    cJSON *j;
    const cJSON *code;
    uint8_t key[32];
    FwPair *p;
    int rc, tries;
    if (!b) return ESP_OK;
    j = cJSON_Parse(b);
    free(b);
    code = cJSON_GetObjectItemCaseSensitive(j, "code");
    if (!cJSON_IsString(code)) { cJSON_Delete(j); return error(req, "400 Bad Request", "code required"); }
    if (!(p = fw_store_pair_lock())) { cJSON_Delete(j); return error(req, "503 Service Unavailable", "starting up; try again"); }
    esp_fill_random(key, sizeof key);
    rc = fw_pair_finish(p, code->valuestring, now_ms(), key, key_hex);
    tries = FW_PAIR_TRIES - p->tries;
    fw_store_pair_unlock(rc == FW_PAIR_OK);
    cJSON_Delete(j);
    memset(key, 0, sizeof key);
    {
        char extra[48];
        snprintf(extra, sizeof extra, "\"ok\":%s", rc == FW_PAIR_OK ? "true" : "false");
        fw_mem_report("pair_finish", extra);
    }
    if (rc == FW_PAIR_NO_CODE) return error(req, "409 Conflict", "no code on the wall; ask for a new one");
    if (rc == FW_PAIR_WRONG) {
        snprintf(out, sizeof out, "{\"error\":\"wrong code\",\"tries_left\":%d}", tries > 0 && tries < FW_PAIR_TRIES ? tries : 0);
        return reply(req, "403 Forbidden", out);
    }
    snprintf(out, sizeof out, "{\"key\":\"%s\"}", key_hex);
    memset(key_hex, 0, sizeof key_hex);
    return reply(req, "200 OK", out);
}

static esp_err_t pair_forget(httpd_req_t *req){
    FwPair *p;
    if (!gate(req)) return ESP_OK;
    if (!(p = fw_store_pair_lock())) return error(req, "503 Service Unavailable", "starting up; try again");
    fw_pair_forget_all(p);
    fw_store_pair_unlock(true);
    fw_mem_report("pair_forgot_all", NULL);
    return reply(req, "200 OK", "{\"signed_in\":false}");
}

/* ------------------------------------------------------------ settings */

/* Built in the shared block (big_buffer.h): settings (up to 4 KiB), airport
 * names (up to 4 KiB) and the reply, instead of ~13 KiB of heap each time the
 * dashboard loads. Wiped before the block is given back. */
static esp_err_t send_settings(httpd_req_t *req, const char *status){
    enum { PART = 4096 };
    uint32_t rev = 0;
    char *b = (char *)fw_big_take(5000), *doc, *aliases, *out;
    size_t cap;
    int n;
    esp_err_t e;
    if (!b) return error(req, "503 Service Unavailable", "busy, try again");
    doc = b; aliases = b + PART; out = b + 2 * PART;
    cap = fw_big_bytes - 2 * PART;
    n = fw_store_settings_copy(doc, PART, &rev);
    if (n < 0 || fw_aliases_json(aliases, PART) < 0) {
        memset(b, 0, 2 * PART);
        fw_big_give();
        return error(req, "500 Internal Server Error", n < 0 ? "settings too large" : "aliases");
    }
    snprintf(out, cap, "{\"revision\":%u,\"settings\":%s,\"aliases\":%s,\"feed\":%d}", (unsigned)rev, n ? doc : "null",
             aliases, (int)fw_feed_state());
    e = reply(req, status, out);
    memset(b, 0, 2 * PART + strlen(out));
    fw_big_give();
    return e;
}

/* Flight stats for the dashboard's Stats tab (ANALYTICS.md). Built in the
 * shared block (big_buffer.h) for the moment it takes, not in 8 KiB of heap. */
static esp_err_t stats_get(httpd_req_t *req){
    size_t cap = 8192, n;          /* at most ~6.6 KB: 48 hours, 31 days of 12 airlines */
    char *b;
    esp_err_t e;
    if (!gate(req)) return ESP_OK;
    if (!(b = (char *)fw_big_take(5000))) return error(req, "503 Service Unavailable", "busy, try again");
    n = fw_stats_json(b, cap);
    e = n < cap ? reply(req, "200 OK", b) : error(req, "500 Internal Server Error", "stats too large");
    fw_big_give();
    return e;
}

/* The wall's recent status lines (wall_log.h) for the dashboard's Console:
 * streamed from RTC memory in 512-byte pieces, no heap. */
static esp_err_t log_get(httpd_req_t *req){
    char chunk[512];
    size_t at = 0, n;
    if (!gate(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while ((n = fw_log_read(at, chunk, sizeof chunk)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) return ESP_FAIL;
        at += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* The owned Sync schedule (SYNC.md "Owned Sync"), built in the shared block. */
static esp_err_t send_sync(httpd_req_t *req){
    char *b;
    size_t n;
    esp_err_t e;
    if (!(b = (char *)fw_big_take(5000))) return error(req, "503 Service Unavailable", "busy, try again");
    n = fw_sync_store_json(b, 8192);
    e = n < 8192 ? reply(req, "200 OK", b) : error(req, "500 Internal Server Error", "schedule too large");
    fw_big_give();
    return e;
}

static esp_err_t sync_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    return send_sync(req);
}

/* The whole schedule each time; flash is written only when it changed. */
static esp_err_t sync_put(httpd_req_t *req){
    char *b, why[80], out[160];
    if (!gate(req)) return ESP_OK;
    if (!(b = body(req))) return ESP_OK;
    if (!fw_sync_store_put(b, why, sizeof why)) {
        free(b);
        snprintf(out, sizeof out, "{\"error\":\"%s\"}", why);
        return reply(req, "400 Bad Request", out);
    }
    free(b);
    return send_sync(req);
}

static esp_err_t settings_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    return send_settings(req, "200 OK");
}

static void now_iso(char *out, size_t cap){
    time_t t = time(NULL);
    struct tm tm;
    if (t < 1700000000) { snprintf(out, cap, "%s", ""); return; }
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static esp_err_t settings_put(httpd_req_t *req){
    char *edit, *cur, *next = NULL, iso[24], out[200];
    uint32_t rev, have;
    FwSettingsError err;
    int rc;
    esp_err_t e;
    if (!gate(req)) return ESP_OK;
    if (!if_match(req, &rev)) return error(req, "428 Precondition Required", "If-Match revision required");
    if (!(edit = body(req))) return ESP_OK;
    cur = fw_store_settings(&have);
    if (rev != have) { free(edit); free(cur); return error(req, "409 Conflict", "changed elsewhere; reload"); }
    now_iso(iso, sizeof iso);
    rc = fw_settings_apply(cur ? cur : fw_settings_default(), edit, iso, &next, &err);
    free(edit);
    free(cur);
    if (rc == FW_SETTINGS_INVALID) {
        snprintf(out, sizeof out, "{\"error\":\"%s\",\"field\":\"%s\"}", err.why, err.field);
        return reply(req, "400 Bad Request", out);
    }
    if (rc != FW_SETTINGS_OK) return error(req, "400 Bad Request", rc == FW_SETTINGS_TOO_LARGE ? "settings too large" : "stored settings unreadable");
    e = fw_store_save(next, &rev);
    free(next);
    if (e == ESP_ERR_INVALID_STATE) return error(req, "409 Conflict", "changed elsewhere; reload");
    if (e != ESP_OK) return error(req, "500 Internal Server Error", "not saved");
    fw_feed_settings_changed();
    return send_settings(req, "200 OK");
}

/* Changes made in the FlightWall app (app_settings.h): how the last check
 * went, and whether a full copy is queued. */
static esp_err_t send_app(httpd_req_t *req, const char *status){
    static const char *const RESULT[] = {"never", "ok", "network", "refused", "not_saved"};
    FwAppStatus a;
    char out[192];
    fw_app_settings_status(&a);
    snprintf(out, sizeof out,
             "{\"pending\":%s,\"result\":\"%s\",\"checked\":%u,\"changed_at\":%u,\"changed\":%d,\"status\":%u}",
             a.pending ? "true" : "false", RESULT[a.result <= FW_APP_NOT_SAVED ? a.result : 0], (unsigned)a.checked,
             (unsigned)a.changed_at, a.changed, a.status);
    return reply(req, status, out);
}

static esp_err_t settings_import_get(httpd_req_t *req){
    char q[16];
    if (!gate(req)) return ESP_OK;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "check=1")) fw_app_settings_ask();
    return send_app(req, "200 OK");
}

/* Copy everything from the app: queued for the feed task, which asks the
 * vendor for the app's settings; the page watches GET for the result. */
static esp_err_t settings_import(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    fw_app_settings_import();
    return send_app(req, "202 Accepted");
}

/* Tracked flights' times from the owner's FlightAware AeroAPI key: whether a
 * key is saved (its last four characters only) and what it has been used for
 * since start. The key itself never leaves the wall. */
static esp_err_t send_tracking(httpd_req_t *req, const char *status){
    char key[FW_AERO_KEY_MAX + 1], out[256];
    FwAeroStatus a;
    bool set = fw_aero_key_load(key, sizeof key);
    size_t n = strlen(key);
    fw_community_aero_status(&a);
    snprintf(out, sizeof out,
             "{\"key_set\":%s,\"key_end\":\"%s\",\"in_use\":%s,\"rejected\":%s,\"flights_calls\":%u,"
             "\"position_calls\":%u,\"last_status\":%u}",
             set ? "true" : "false", set && n >= 4 ? key + n - 4 : "", a.in_use ? "true" : "false",
             a.rejected ? "true" : "false", a.flights_calls, a.position_calls, a.last_status);
    memset(key, 0, sizeof key);
    return reply(req, status, out);
}

static esp_err_t tracking_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    return send_tracking(req, "200 OK");
}

/* {"aeroapi_key":"..."} saves it, {"aeroapi_key":null} removes it. Flash is
 * written only here, when the owner saves or removes a key. */
static esp_err_t tracking_put(httpd_req_t *req){
    char *b;
    cJSON *j;
    const cJSON *k;
    esp_err_t e;
    if (!gate(req)) return ESP_OK;
    if (!(b = body(req))) return ESP_OK;
    j = cJSON_Parse(b);
    memset(b, 0, strlen(b));
    free(b);
    k = cJSON_GetObjectItemCaseSensitive(j, "aeroapi_key");
    if (!cJSON_IsObject(j) || !(cJSON_IsNull(k) || cJSON_IsString(k))) {
        cJSON_Delete(j);
        return error(req, "400 Bad Request", "send {\"aeroapi_key\": \"...\"} or null");
    }
    e = fw_aero_key_save(cJSON_IsString(k) ? k->valuestring : NULL);
    if (cJSON_IsString(k)) memset(k->valuestring, 0, strlen(k->valuestring));
    cJSON_Delete(j);
    if (e == ESP_ERR_INVALID_ARG) return error(req, "400 Bad Request", "a key is 8 to 64 letters, digits, - or _");
    if (e != ESP_OK) return error(req, "500 Internal Server Error", "not saved");
    fw_feed_settings_changed();                       /* the feed task picks the key up at once */
    return send_tracking(req, "200 OK");
}

/* {"revision":n,"aliases":[...]} */
static esp_err_t aliases_put(httpd_req_t *req){
    char *b, *arr;
    cJSON *j;
    const cJSON *rev, *list;
    uint32_t next = 0;
    int rc;
    if (!gate(req)) return ESP_OK;
    if (!(b = body(req))) return ESP_OK;
    j = cJSON_Parse(b);
    free(b);
    rev = cJSON_GetObjectItemCaseSensitive(j, "revision");
    list = cJSON_GetObjectItemCaseSensitive(j, "aliases");
    if (!cJSON_IsNumber(rev) || !cJSON_IsArray(list) || cJSON_GetArraySize(list) > OA_MAX_ALIASES) {
        cJSON_Delete(j);
        return error(req, "400 Bad Request", "revision and at most 16 aliases required");
    }
    arr = cJSON_PrintUnformatted(list);
    rc = arr ? fw_aliases_save(arr, (uint32_t)rev->valuedouble, &next) : OA_STORAGE;
    cJSON_free(arr);
    cJSON_Delete(j);
    if (rc == OA_STALE) return error(req, "409 Conflict", "changed elsewhere; reload");
    if (rc == OA_INVALID) return error(req, "400 Bad Request", "check each alias: codes are 3 or 4 capital letters, text up to 31 plain characters, no duplicates");
    if (rc != OA_OK) return error(req, "500 Internal Server Error", "not saved");
    fw_feed_settings_changed();
    return send_settings(req, "200 OK");
}

/* ------------------------------------------------------------ the display preview */

static void add_str(cJSON *o, const char *k, const char *v){ if (v[0]) cJSON_AddStringToObject(o, k, v); }
static void add_num(cJSON *o, const char *k, double v){ if (!isnan(v)) cJSON_AddNumberToObject(o, k, v); }
static void add_flag(cJSON *o, const char *k, bool v){ if (v) cJSON_AddTrueToObject(o, k); }

/* The flights the wall has now, for the dashboard's preview (DASHBOARD.md
 * "Preview"): {"state":n,"tracked":b,"overflow":n,"flights":[{FwFlight's
 * fields by name; empty and unknown ones left out}]}. */
static esp_err_t flights_get(httpd_req_t *req){
    FwFeedFrame *fr;
    cJSON *root, *list;
    char *out;
    esp_err_t r;
    if (!gate(req)) return ESP_OK;
    if (!(fr = malloc(sizeof *fr))) return error(req, "500 Internal Server Error", "out of memory");
    fw_feed_copy(fr);
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", (int)fw_feed_state());
    cJSON_AddBoolToObject(root, "tracked", fr->tracked);
    cJSON_AddNumberToObject(root, "overflow", fr->overflow);
    list = cJSON_AddArrayToObject(root, "flights");
    for (int i = 0; i < fr->count && list; ++i) {
        const FwFlight *f = &fr->flights[i];
        cJSON *o = cJSON_CreateObject();
        char sts[2] = {f->sts, 0};
        if (!o) break;
        add_str(o, "icao", f->icao); add_str(o, "cs", f->cs); add_str(o, "reg", f->reg);
        add_num(o, "alt_b", f->alt_b); add_num(o, "alt_g", f->alt_g); add_num(o, "vel", f->vel);
        add_num(o, "trk", f->trk); add_num(o, "vr", f->vr); add_flag(o, "gnd", f->gnd);
        add_str(o, "sqk", f->sqk);
        add_str(o, "orig_icao", f->orig_icao); add_str(o, "orig_iata", f->orig_iata);
        add_str(o, "dest_icao", f->dest_icao); add_str(o, "dest_iata", f->dest_iata);
        add_str(o, "orig", f->orig); add_str(o, "dest", f->dest);
        add_str(o, "al_code", f->al_code); add_str(o, "al_paint", f->al_paint); add_str(o, "al_op", f->al_op);
        add_str(o, "al_name", f->al_name);
        add_str(o, "ac_type", f->ac_type); add_str(o, "ac_name", f->ac_name); add_str(o, "ac_class", f->ac_class);
        add_flag(o, "has_trk", f->has_trk); add_str(o, "sts", sts);
        if (f->sexp) cJSON_AddNumberToObject(o, "sexp", (double)f->sexp);
        add_num(o, "prog", f->prog); add_num(o, "dist", f->dist);
        add_str(o, "conf", f->conf); add_str(o, "dep", f->dep); add_str(o, "arr", f->arr);
        add_str(o, "over", f->over); add_str(o, "tft", f->tft);
        add_flag(o, "trk_gnd", f->trk_gnd); add_flag(o, "trk_metrics", f->trk_metrics);
        cJSON_AddItemToArray(list, o);
    }
    free(fr);
    out = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!out) return error(req, "500 Internal Server Error", "out of memory");
    r = reply(req, "200 OK", out);
    cJSON_free(out);
    return r;
}

/* %XX and + in a query value, in place. */
static void url_decode(char *s){
    char *o = s;
    for (; *s; ++s) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool logo_exists(void *ctx, const char *path){ (void)ctx; return fw_assets_exists(path); }

/* GET /api/logo?al=<airline code>&ac=<aircraft class>: the logo file the wall
 * itself would draw for that flight (LOGOS.md), resolved and read from the
 * asset partition like display.cpp's decode_logo; 404 when there is none. */
static esp_err_t logo_get(httpd_req_t *req){
    char q[96], al[8] = "", ac[32] = "", path_buf[FW_LOGO_PATH_MAX];
    const char *path;
    uint8_t *buf;
    esp_err_t r;
    int n;
    if (!gate(req)) return ESP_OK;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "al", al, sizeof al) != ESP_OK) al[0] = 0;
        if (httpd_query_key_value(q, "ac", ac, sizeof ac) != ESP_OK) ac[0] = 0;
        url_decode(al);
        url_decode(ac);
    }
    if (!fw_assets_mount_partition()) return error(req, "404 Not Found", "no logos");
    path = fw_logo_resolve(al, ac, logo_exists, NULL, path_buf, sizeof path_buf);
    if (!path) return error(req, "404 Not Found", "no logo");
    if (!(buf = malloc(4096))) return error(req, "500 Internal Server Error", "out of memory");
    n = fw_assets_read_file(path, buf, 4096);       /* the largest logo is 2,907 bytes */
    if (n <= 0) { free(buf); return error(req, "404 Not Found", "no logo"); }
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    r = httpd_resp_send(req, (const char *)buf, n);
    free(buf);
    return r;
}

void fw_dashboard_api_register(httpd_handle_t h){
    static const httpd_uri_t routes[] = {
        {.uri = "/api/session", .method = HTTP_GET, .handler = session_get},
        {.uri = "/api/pair/start", .method = HTTP_POST, .handler = pair_start},
        {.uri = "/api/pair/finish", .method = HTTP_POST, .handler = pair_finish},
        {.uri = "/api/pair/forget", .method = HTTP_POST, .handler = pair_forget},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get},
        {.uri = "/api/settings", .method = HTTP_PUT, .handler = settings_put},
        {.uri = "/api/settings/import", .method = HTTP_POST, .handler = settings_import},
        {.uri = "/api/settings/import", .method = HTTP_GET, .handler = settings_import_get},
        {.uri = "/api/aliases", .method = HTTP_PUT, .handler = aliases_put},
        {.uri = "/api/stats", .method = HTTP_GET, .handler = stats_get},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/api/sync", .method = HTTP_GET, .handler = sync_get},
        {.uri = "/api/sync", .method = HTTP_PUT, .handler = sync_put},
        {.uri = "/api/tracking", .method = HTTP_GET, .handler = tracking_get},
        {.uri = "/api/tracking", .method = HTTP_PUT, .handler = tracking_put},
        {.uri = "/api/flights", .method = HTTP_GET, .handler = flights_get},
        {.uri = "/api/logo", .method = HTTP_GET, .handler = logo_get},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) httpd_register_uri_handler(h, &routes[i]);
}

bool fw_dashboard_pair_code(char code[7], int *seconds_left){
    FwPair *p = fw_store_pair_lock();
    const char *c = p ? fw_pair_code(p, now_ms(), seconds_left) : NULL;
    if (c) memcpy(code, c, 7);
    fw_store_pair_unlock(false);
    return c != NULL;
}
