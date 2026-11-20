/* Flight stats on the board; see stats.h and ANALYTICS.md. */
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp32_transport.h"
#include "big_buffer.h"
#include "fw_analytics.h"
#include "fw_clock.h"
#include "fw_feed.h"
#include "feed.h"
#include "memory_probe.h"

static FwAnalytics *s_an;          /* ~15.5 KiB, allocated once at start */
static SemaphoreHandle_t s_lock;
static uint32_t s_now;             /* set by fw_stats_begin, used by its flights */
static int s_offset;
static bool s_ready;

/* Optional LAN/NAS HTTP checkpoint backup (plain HTTP on local network). */
static char s_sync_url[100];
static volatile bool s_sync_want;
static bool s_sync_restored, s_sync_tried_restore;
static uint32_t s_sync_saved_sightings;
static uint32_t s_sync_last_ms;
static FwStatsSyncStatus s_sync_st;

#define CLOCK_SET 1700000000u
#define SYNC_EVERY_MS (15u * 60u * 1000u)
#define SYNC_IO_MS 6000u

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

void fw_stats_init(void){
    if (s_an) return;
    s_lock = xSemaphoreCreateMutex();
    s_an = heap_caps_malloc(sizeof *s_an, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_lock || !s_an) { fw_mem_report("stats_no_memory", NULL); return; }
    fw_an_init(s_an);
}

void fw_stats_set_area(double lat, double lon, double radius_km){
    if (!s_an) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_set_area(s_an, lat, lon, radius_km);
    xSemaphoreGive(s_lock);
}

void fw_stats_set_bounds(double lat_min, double lat_max, double lon_min, double lon_max){
    if (!s_an) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_set_bounds(s_an, lat_min, lat_max, lon_min, lon_max);
    xSemaphoreGive(s_lock);
}

void fw_stats_set_sync_url(const char *url){
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!url || strncmp(url, "http://", 7) != 0) {
        s_sync_url[0] = 0;
        s_sync_st.configured = false;
    } else {
        if (strcmp(s_sync_url, url) != 0) {
            snprintf(s_sync_url, sizeof s_sync_url, "%s", url);
            s_sync_tried_restore = false;
        }
        s_sync_st.configured = true;
    }
    xSemaphoreGive(s_lock);
}

/* The settings' zone at `now`: standard offset, plus an hour inside daylight
 * saving; no zone = UTC. */
static int local_offset(uint32_t now){
    static FwFeedDisplay d;          /* feed task only */
    FwZone z;
    FwTime t;
    fw_feed_display_copy(&d);
    if (d.utc_offset_min == FW_FEED_ZONE_UNSET) return 0;
    z.standard_offset_min = d.utc_offset_min;
    z.dst_rule = !strcmp(d.dst_rule, "north_america") ? FW_DST_NORTH_AMERICA
               : !strcmp(d.dst_rule, "europe") ? FW_DST_EUROPE : FW_DST_NONE;
    return fw_local_time((int64_t)now, z, &t) && t.dst ? d.utc_offset_min + 60 : d.utc_offset_min;
}

bool fw_stats_begin(void){
    time_t now = time(NULL);
    s_ready = s_an && now >= (time_t)CLOCK_SET;
    if (!s_ready) return false;
    s_now = (uint32_t)now;
    s_offset = local_offset(s_now);
    return true;
}

static bool letters3(const char *s){
    return s && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] >= 'A' && s[2] <= 'Z';
}

void fw_stats_flight_ex(const char *hex, const char *callsign, const char *reg, const char *airline,
                        const char *ac_type, const char *orig, const char *dest,
                        bool has_pos, double lat, double lon){
    char code[FW_AN_CODE] = {0};
    FwAnObs obs;
    if (!s_ready) return;
    if (letters3(airline) && !airline[3]) memcpy(code, airline, 3);
    else if (letters3(callsign) && callsign[3] >= '0' && callsign[3] <= '9') memcpy(code, callsign, 3);
    memset(&obs, 0, sizeof obs);
    obs.key = fw_an_flight_key(hex, callsign, reg);
    obs.airline = code[0] ? code : NULL;
    obs.ac_type = ac_type && *ac_type ? ac_type : NULL;
    obs.orig = orig && *orig ? orig : NULL;
    obs.dest = dest && *dest ? dest : NULL;
    obs.callsign = callsign;
    obs.reg = reg;
    obs.has_pos = has_pos;
    obs.lat = lat;
    obs.lon = lon;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_observe_ex(s_an, &obs, s_now, s_offset);
    xSemaphoreGive(s_lock);
}

void fw_stats_flight(const char *hex, const char *callsign, const char *reg, const char *airline){
    fw_stats_flight_ex(hex, callsign, reg, airline, NULL, NULL, NULL, false, 0, 0);
}

void fw_stats_route(const char *hex, const char *callsign, const char *reg, const char *orig, const char *dest){
    uint32_t key;
    time_t now = time(NULL);
    if (!s_an || now < (time_t)CLOCK_SET) return;
    key = fw_an_flight_key(hex, callsign, reg);
    if (!key) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_route(s_an, key, orig, dest, (uint32_t)now, s_offset);
    xSemaphoreGive(s_lock);
}

void fw_stats_position(const char *hex, const char *callsign, const char *reg, double lat, double lon){
    uint32_t key;
    time_t now = time(NULL);
    if (!s_an || now < (time_t)CLOCK_SET) return;
    key = fw_an_flight_key(hex, callsign, reg);
    if (!key) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_position(s_an, key, lat, lon, (uint32_t)now);
    xSemaphoreGive(s_lock);
}

void fw_stats_covered(void){
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_covered(s_an, s_now, s_offset);
    xSemaphoreGive(s_lock);
}

typedef struct { FwStatsEmit emit; void *ctx; bool ok; } EmitWrap;
static void wrap_emit(void *ctx, const char *data, size_t len){
    EmitWrap *w = (EmitWrap *)ctx;
    if (w->ok && !w->emit(w->ctx, data, len)) w->ok = false;
}

bool fw_stats_stream(FwStatsEmit emit, void *ctx){
    time_t now = time(NULL);
    bool set = now >= (time_t)CLOCK_SET;
    char head[256];
    int n;
    EmitWrap w = {emit, ctx, true};
    if (!s_an) {
        const char *empty = "{\"hour\":-1,\"day\":-1,\"offset\":0,\"sync\":{\"configured\":false},\"stats\":{\"hours\":[],\"days\":[],\"rare\":[],\"rare_total\":0,\"heat\":null}}";
        return emit(ctx, empty, strlen(empty));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = snprintf(head, sizeof head,
                 "{\"hour\":%ld,\"day\":%ld,\"offset\":%d,"
                 "\"sync\":{\"configured\":%s,\"pending\":%s,\"ok\":%s,\"restored\":%s,\"status\":%d,\"at\":%u},"
                 "\"stats\":",
                 set ? (long)fw_an_local_hour((uint32_t)now, s_offset) : -1L,
                 set ? (long)fw_an_local_day((uint32_t)now, s_offset) : -1L, s_offset,
                 s_sync_st.configured ? "true" : "false", s_sync_st.pending ? "true" : "false",
                 s_sync_st.ok ? "true" : "false", s_sync_st.restored ? "true" : "false",
                 s_sync_st.status, (unsigned)s_sync_st.last_at);
    if (n > 0 && (size_t)n < sizeof head) wrap_emit(&w, head, (size_t)n);
    fw_an_json_stream(s_an, wrap_emit, &w);
    xSemaphoreGive(s_lock);
    wrap_emit(&w, "}", 1);
    return w.ok;
}

typedef struct { char *buf; size_t cap, len; } BufSink;
static bool buf_sink(void *ctx, const char *data, size_t len){
    BufSink *b = (BufSink *)ctx;
    if (b->buf && b->len + 1 < b->cap) {
        size_t room = b->cap - 1 - b->len;
        size_t take = len < room ? len : room;
        memcpy(b->buf + b->len, data, take);
    }
    b->len += len;
    return true;
}

size_t fw_stats_json(char *buf, size_t cap){
    BufSink b = {buf, cap, 0};
    if (buf && cap) buf[0] = 0;
    fw_stats_stream(buf_sink, &b);
    if (buf && cap) buf[b.len < cap ? b.len : cap - 1] = 0;
    return b.len;
}

size_t fw_stats_checkpoint_save(uint8_t *buf, size_t cap){
    size_t n;
    if (!s_an) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = fw_an_save(s_an, buf, cap);
    xSemaphoreGive(s_lock);
    return n;
}

bool fw_stats_checkpoint_load(const uint8_t *buf, size_t len){
    bool ok;
    if (!s_an) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = fw_an_load(s_an, buf, len);
    if (ok) s_sync_saved_sightings = s_an->total_sightings;
    xSemaphoreGive(s_lock);
    return ok;
}

/* ------------------------------------------------------------ LAN/NAS HTTP backup */

void fw_stats_sync_request(void){
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sync_url[0]) {
        s_sync_want = true;
        s_sync_st.pending = true;
    }
    xSemaphoreGive(s_lock);
    fw_feed_settings_changed();
}

void fw_stats_sync_status(FwStatsSyncStatus *out){
    memset(out, 0, sizeof *out);
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_sync_st;
    xSemaphoreGive(s_lock);
}

/* Parse "http://host[:port][/base]" into host, port (default "80"), path + "/checkpoint". */
static bool parse_url(const char *url, char *host, size_t hcap, char *port, size_t pcap, char *path, size_t pathcap){
    const char *p, *slash, *colon;
    size_t hlen, plen;
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    p = url + 7;
    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - p);
        plen = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (!plen || plen >= pcap) return false;
        memcpy(port, colon + 1, plen);
        port[plen] = 0;
    } else {
        hlen = slash ? (size_t)(slash - p) : strlen(p);
        snprintf(port, pcap, "80");
    }
    if (!hlen || hlen >= hcap) return false;
    memcpy(host, p, hlen);
    host[hlen] = 0;
    if (slash) {
        size_t blen = strlen(slash);
        while (blen > 0 && slash[blen - 1] == '/') --blen;
        snprintf(path, pathcap, "%.*s/checkpoint", (int)blen, slash);
    } else {
        snprintf(path, pathcap, "/checkpoint");
    }
    return true;
}

static bool send_all(FwEspTransport *t, const uint8_t *buf, size_t len){
    size_t sent = 0;
    uint32_t last = now_ms();
    while (sent < len) {
        int n = fw_esp_transport_send(t, buf + sent, len - sent, 1000);
        if (n > 0) { sent += (size_t)n; last = now_ms(); continue; }
        if (n != -2 || now_ms() - last > SYNC_IO_MS) return false;
    }
    return true;
}

static int http_status_and_body(FwEspTransport *t, uint8_t *body, size_t cap, size_t *out_len){
    static uint8_t chunk[256];
    char hdr[384];
    size_t hlen = 0, blen = 0;
    bool in_body = false;
    int status = 0;
    long content_len = -1;
    uint32_t last = now_ms();
    if (out_len) *out_len = 0;
    for (;;) {
        int n = fw_esp_transport_receive(t, chunk, sizeof chunk, 1000);
        if (n > 0) {
            last = now_ms();
            for (int i = 0; i < n; ++i) {
                if (!in_body) {
                    if (hlen + 1 < sizeof hdr) hdr[hlen++] = (char)chunk[i];
                    hdr[hlen] = 0;
                    if (hlen >= 4 && memcmp(hdr + hlen - 4, "\r\n\r\n", 4) == 0) {
                        char *cl;
                        sscanf(hdr, "HTTP/%*d.%*d %d", &status);
                        cl = strstr(hdr, "Content-Length:");
                        if (!cl) cl = strstr(hdr, "content-length:");
                        if (cl) content_len = strtol(cl + 15, NULL, 10);
                        in_body = true;
                        if (content_len == 0) goto done;
                    }
                } else {
                    if (body && blen < cap) body[blen] = chunk[i];
                    ++blen;
                    if (content_len >= 0 && (long)blen >= content_len) goto done;
                    if (body && blen > cap) return -1;
                }
            }
            continue;
        }
        if (n == 0) break;
        if (n != -2 || now_ms() - last > SYNC_IO_MS) return -1;
    }
done:
    if (out_len) *out_len = blen;
    return status;
}

static void sync_transfer(const char *url, bool restore){
    char host[64], port[8], path[96], req[256];
    FwEspTransport t;
    uint8_t *buf;
    size_t len = 0;
    int status = 0;
    bool ok = false;
    if (!parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path)) return;
    buf = fw_big_take(0);
    if (!buf || fw_big_bytes < FW_AN_BLOB_MAX) { if (buf) fw_big_give(); return; }
    if (!restore) {
        len = fw_stats_checkpoint_save(buf, FW_AN_BLOB_MAX);
        if (!len) { fw_big_give(); return; }
    }
    fw_esp_transport_init(&t);
    if (fw_esp_transport_connect(&t, host, port, SYNC_IO_MS)) {
        int rlen;
        if (restore) {
            rlen = snprintf(req, sizeof req,
                            "GET %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n\r\n", path, host, port);
            if (rlen > 0 && (size_t)rlen < sizeof req && send_all(&t, (const uint8_t *)req, (size_t)rlen)) {
                status = http_status_and_body(&t, buf, FW_AN_BLOB_MAX, &len);
                if (status == 200 && len > 0) ok = fw_stats_checkpoint_load(buf, len);
            }
        } else {
            rlen = snprintf(req, sizeof req,
                            "PUT %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/octet-stream\r\n"
                            "Content-Length: %u\r\nConnection: close\r\n\r\n",
                            path, host, port, (unsigned)len);
            if (rlen > 0 && (size_t)rlen < sizeof req && send_all(&t, (const uint8_t *)req, (size_t)rlen) &&
                send_all(&t, buf, len)) {
                status = http_status_and_body(&t, NULL, 0, NULL);
                ok = status >= 200 && status < 300;
            }
        }
        fw_esp_transport_close(&t);
    }
    fw_big_give();
    {
        time_t now = time(NULL);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_sync_st.pending = false;
        s_sync_st.status = status;
        if (now >= (time_t)CLOCK_SET) s_sync_st.last_at = (uint32_t)now;
        if (restore) {
            if (ok) { s_sync_restored = true; s_sync_st.restored = true; s_sync_st.ok = true; }
        } else {
            s_sync_st.ok = ok;
            if (ok && s_an) s_sync_saved_sightings = s_an->total_sightings;
        }
        xSemaphoreGive(s_lock);
    }
    {
        char extra[96];
        snprintf(extra, sizeof extra, "\"op\":\"%s\",\"ok\":%s,\"status\":%d,\"bytes\":%u",
                 restore ? "restore" : "backup", ok ? "true" : "false", status, (unsigned)len);
        fw_mem_report("stats_sync", extra);
    }
}

void fw_stats_sync_step(void){
    char url[100];
    bool do_restore = false, do_backup = false;
    uint32_t now = now_ms(), sightings = 0;
    if (!s_an || !s_lock || time(NULL) < (time_t)CLOCK_SET) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(url, sizeof url, "%s", s_sync_url);
    sightings = s_an->total_sightings;
    if (url[0]) {
        if (!s_sync_tried_restore) {
            s_sync_tried_restore = true;
            do_restore = true;
        } else if (s_sync_want) {
            s_sync_want = false;
            do_backup = true;
        } else if (sightings != s_sync_saved_sightings && (!s_sync_last_ms || now - s_sync_last_ms >= SYNC_EVERY_MS)) {
            do_backup = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (!url[0] || (!do_restore && !do_backup)) return;
    s_sync_last_ms = now ? now : 1;
    sync_transfer(url, do_restore);
}
