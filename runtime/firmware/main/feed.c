/* The live flight feed on the board; see feed.h and FEED.md.
 *
 * What it does, as stock does (FUN_4201563c / FUN_42014fe0):
 *  - POST http://api.theflightwall.com/v2/flights with the stored
 *    configuration's request body, Content-Type: application/json,
 *    Accept: application/x-msgpack and x-api-key from NVS api/key;
 *  - every 10 s; only a 200 with a MessagePack body of at most 5 KiB counts;
 *    on any failure the last good list stays on screen.
 * Differences: one connection per poll (stock keeps one alive); a backoff
 * after repeated failures (20 s, 40 s, at most 60 s); the request comes from
 * the owned settings and is rebuilt when they change. Reads NVS only; never
 * logs the key, the body or the response.
 *
 * Owned: owned_config.flight_source "community" swaps the vendor poll for the
 * community source (community.cpp: adsb.fi positions, adsbdb routes), which
 * needs no vendor key. Switching clears the other source's flights, so one
 * source's list is never shown as the other's. */
#include "feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp32_transport.h"
#include "memory_probe.h"
#include "wifi_link.h"
#include "settings_store.h"
#include "community.h"
#include "stats.h"
#include "app_settings.h"
#include "sync_store.h"
#include "position_fix.h"
#include "fw_airports.h"
#include "cJSON.h"
#include <math.h>
#include <strings.h>
#include <time.h>

#define HOST "api.theflightwall.com"
#define POLL_MS 10000
#define IO_MS 10000

static SemaphoreHandle_t s_lock;
static volatile FwFeedState s_state = FW_FEED_OFF;
static FwFeedDisplay s_display;
static volatile uint32_t s_display_serial;
static char s_key[80];
static uint32_t s_store_serial;
static TaskHandle_t s_task;
static FwFeedFrame s_frame, s_parse;
static volatile uint32_t s_serial;
static char *s_request;                 /* headers + body, built once */
static size_t s_request_len;
static uint8_t s_body[FW_FEED_RESPONSE_MAX];
static unsigned s_polls, s_fails, s_streak;
static bool s_has_key;
static FwFlightSource s_source;
static bool s_follow = true;            /* owned_config.follow_app */
static char s_sync_flight[16];          /* the Sync leg asked for as a tracked flight, "" none */

/* Owned: the progress bar on the FlightWall feed (FLIGHT-SCREENS.md
 * "Progress bar"), which sends no positions: the shown flights' positions from
 * adsb.fi by ICAO address (position_fix.h), at most one lookup per poll, for
 * the flight whose position is oldest, each flight at most every 50 s, and
 * only when both its airports are in the airport table. A position counts for
 * 3 minutes. Static, five slots: no heap. */
#define FIX_EVERY_MS 50000
#define FIX_VALID_MS 180000
typedef struct { char hex[8]; float lat, lon; bool ground, ok; uint32_t at; } Fix;
static Fix s_fix[FW_FEED_MAX_FLIGHTS];
static unsigned s_fixes, s_fix_fails;
static FwWatchArea s_areas[5];          /* the owner's areas, for flights on the ground */
static int s_area_count;

FwFeedState fw_feed_state(void){ return s_state; }
uint32_t fw_feed_display_serial(void){ return s_display_serial; }

uint32_t fw_feed_display_copy(FwFeedDisplay *out){
    uint32_t serial;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_display, sizeof *out);
    serial = s_display_serial;
    xSemaphoreGive(s_lock);
    return serial;
}

void fw_feed_settings_changed(void){ if (s_task) xTaskNotifyGive(s_task); }
uint32_t fw_feed_serial(void){ return s_serial; }

uint32_t fw_feed_copy(FwFeedFrame *out){
    uint32_t serial;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_frame, sizeof *out);
    serial = s_serial;
    xSemaphoreGive(s_lock);
    return serial;
}

bool fw_feed_flight(int i, FwFlight *out, int *count, bool *tracked, int *overflow){
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = i >= 0 && i < s_frame.count;
    if (ok) memcpy(out, &s_frame.flights[i], sizeof *out);
    if (count) *count = s_frame.count;
    if (tracked) *tracked = s_frame.tracked;
    if (overflow) *overflow = s_frame.overflow;
    xSemaphoreGive(s_lock);
    return ok;
}

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

static Fix *fix_for(const char *hex){
    if (strlen(hex) != 6) return NULL;
    for (int i = 0; i < FW_FEED_MAX_FLIGHTS; ++i)
        if (s_fix[i].hex[0] && !strcasecmp(s_fix[i].hex, hex)) return &s_fix[i];
    return NULL;
}

static bool route_known(const FwFlight *f, double *olat, double *olon, double *dlat, double *dlon){
    return fw_airport_position(f->orig_icao, olat, olon) && fw_airport_position(f->dest_icao, dlat, dlon);
}

/* A flight on the ground at one of its airports needs no lookup
 * (fw_ground_progress): NAN when that cannot tell. */
static double ground_progress(const FwFlight *f){
    return f->gnd ? fw_ground_progress(f->orig_icao, f->dest_icao, s_areas, s_area_count) : NAN;
}

/* The FlightWall list's progress from the fresh positions; s_lock held. */
static void apply_progress(FwFeedFrame *fr){
    uint32_t now = now_ms();
    if (!s_display.progress_bar || fr->tracked) return;      /* tracked flights bring their own */
    for (int i = 0; i < fr->count; ++i) {
        FwFlight *f = &fr->flights[i];
        const Fix *x = fix_for(f->icao);
        double olat, olon, dlat, dlon;
        if (x && x->ok && now - x->at < FIX_VALID_MS && route_known(f, &olat, &olon, &dlat, &dlon))
            f->prog = fw_route_progress(olat, olon, dlat, dlon, x->lat, x->lon, x->ground || f->gnd);
        else
            f->prog = ground_progress(f);
    }
}

/* One position lookup for the progress bar, when one is due. */
static void fix_step(void){
    char hex[8] = "";
    uint32_t now = now_ms(), oldest = 0;
    double lat, lon, a, b, c, d;
    bool ground;
    unsigned status;
    int rc;
    Fix *slot = NULL;
    if (s_source != FW_SOURCE_VENDOR || !s_display.progress_bar) return;
    /* flights in the air first; one on the ground only when its airport does
     * not already tell (ground_progress), and only when no airborne one is due */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int pass = 0; pass < 2 && !hex[0]; ++pass) {
        for (int i = 0; i < s_frame.count && !s_frame.tracked; ++i) {
            const FwFlight *f = &s_frame.flights[i];
            const Fix *x = fix_for(f->icao);
            uint32_t age = x ? now - x->at : UINT32_MAX;
            if (f->gnd != (pass == 1) || (f->gnd && ground_progress(f) == ground_progress(f))) continue;
            if (strlen(f->icao) == 6 && age >= FIX_EVERY_MS && age > oldest && route_known(f, &a, &b, &c, &d)) {
                oldest = age;
                snprintf(hex, sizeof hex, "%s", f->icao);
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (!hex[0]) return;
    for (int i = 0; i < 6; ++i) hex[i] = (char)(hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] + 32 : hex[i]);
    rc = fw_position_fix(hex, &lat, &lon, &ground, &status);
    /* its own slot, else one no shown flight uses, else the oldest */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!(slot = fix_for(hex))) {
        for (int i = 0; i < FW_FEED_MAX_FLIGHTS && !slot; ++i) {
            bool used = false;
            for (int k = 0; k < s_frame.count; ++k) used |= s_fix[i].hex[0] && !strcasecmp(s_fix[i].hex, s_frame.flights[k].icao);
            if (!used) slot = &s_fix[i];
        }
        if (!slot) {
            slot = &s_fix[0];
            for (int i = 1; i < FW_FEED_MAX_FLIGHTS; ++i) if (s_fix[i].at < slot->at) slot = &s_fix[i];
        }
    }
    snprintf(slot->hex, sizeof slot->hex, "%s", hex);
    slot->at = now_ms();
    slot->ok = rc == 0;
    if (!rc) {
        slot->lat = (float)lat;
        slot->lon = (float)lon;
        slot->ground = ground;
        apply_progress(&s_frame);
        ++s_serial;
    }
    xSemaphoreGive(s_lock);
    if (rc) ++s_fix_fails;
    if (++s_fixes <= 3 || s_fixes % 50 == 0 || (rc && rc != 11)) {
        char extra[96];
        snprintf(extra, sizeof extra, "\"n\":%u,\"rc\":%d,\"status\":%u,\"fails\":%u", s_fixes, rc, status, s_fix_fails);
        fw_mem_report("progress_fix", extra);              /* counts only: no positions or callsigns */
    }
}

/* NVS api/key, read-only. */
static bool load_key(char *key, size_t key_cap){
    nvs_handle_t h;
    size_t n = key_cap;
    bool ok = false;
    if (nvs_open("api", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_str(h, "key", key, &n) == ESP_OK && n > 1;
        nvs_close(h);
    }
    return ok;
}

static bool build_request(const char *key, const char *config){
    static char body[3072];
    char head[512];
    int blen = fw_feed_body_sync(config, s_sync_flight[0] ? s_sync_flight : NULL, body, sizeof body), hlen;
    if (blen < 0) return false;
    hlen = snprintf(head, sizeof head,
                    "POST /v2/flights HTTP/1.1\r\n"
                    "Host: " HOST "\r\n"
                    "User-Agent: ESP32HTTPClient\r\n"
                    "Connection: close\r\n"
                    "Accept-Encoding: identity;q=1,chunked;q=0.1,*;q=0\r\n"
                    "Content-Type: application/json\r\n"
                    "Accept: application/x-msgpack\r\n"
                    "x-api-key: %s\r\n"
                    "Content-Length: %d\r\n\r\n",
                    key, blen);
    if (hlen <= 0 || (size_t)hlen >= sizeof head) return false;
    free(s_request);
    s_request_len = 0;
    s_request = malloc((size_t)hlen + (size_t)blen);
    if (!s_request) return false;
    memcpy(s_request, head, (size_t)hlen);
    memcpy(s_request + hlen, body, (size_t)blen);
    s_request_len = (size_t)hlen + (size_t)blen;
    memset(head, 0, sizeof head);
    return true;
}

/* One poll. Returns 0 on a good response, else a failure stage (for logs). */
static int poll_once(int *status, size_t *bytes){
    FwEspTransport t;
    FwHttpResponse r;
    size_t sent = 0;
    uint32_t last = now_ms();
    int rc = 0, stage = 0;
    static uint8_t chunk[512];
    *status = 0;
    *bytes = 0;
    fw_esp_transport_init(&t);
    if (!fw_esp_transport_connect(&t, HOST, "80", IO_MS)) return 1;
    while (sent < s_request_len) {
        int n = fw_esp_transport_send(&t, (const uint8_t *)s_request + sent, s_request_len - sent, 1000);
        if (n > 0) { sent += (size_t)n; last = now_ms(); continue; }
        if (n != -2 || now_ms() - last > IO_MS) { stage = 2; goto done; }
    }
    fw_http_init(&r, s_body, sizeof s_body);
    last = now_ms();
    for (;;) {
        int n = fw_esp_transport_receive(&t, chunk, sizeof chunk, 1000);
        if (n > 0) {
            last = now_ms();
            rc = fw_http_feed(&r, chunk, (size_t)n);
            if (rc) break;
            continue;
        }
        if (n == 0 || n != -2 || now_ms() - last > IO_MS) { rc = -1; break; }
    }
    *status = r.status;
    *bytes = r.body_len;
    if (rc != 1) { stage = 3; goto done; }
    if (r.status != 200) { stage = 4; goto done; }
    if (!r.msgpack) { stage = 5; goto done; }                /* JSON answers are not read yet */
    if (fw_feed_parse(s_body, r.body_len, &s_parse) != FW_FEED_OK) { stage = 6; goto done; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_frame, &s_parse, sizeof s_frame);
    apply_progress(&s_frame);
    ++s_serial;
    xSemaphoreGive(s_lock);
    s_state = FW_FEED_LIVE;
    fw_app_settings_hash(s_parse.config_hash);          /* stock: a new hash means the app changed something */
    /* Stats: the listed flights (at most five; the rest only as a count) and
     * this minute watched. A tracked-flight answer is not nearby traffic. */
    if (fw_stats_begin()) {
        if (!s_parse.tracked)
            for (int i = 0; i < s_parse.count; ++i)
                fw_stats_flight(s_parse.flights[i].icao, s_parse.flights[i].cs, s_parse.flights[i].reg,
                                s_parse.flights[i].al_code);
        fw_stats_covered();
    }
done:
    fw_esp_transport_close(&t);
    return stage;
}

/* (Re)reads the owned settings: the source, its request and the display
 * settings. */
/* request_config.radius_requests (or the older single radius_request), for
 * the progress bar of flights on the ground (fw_ground_progress). */
static void read_areas(const char *doc){
    cJSON *root = doc ? cJSON_Parse(doc) : NULL, *rc = cJSON_GetObjectItemCaseSensitive(root, "request_config"), *z;
    cJSON *list = cJSON_GetObjectItemCaseSensitive(rc, "radius_requests"), *one = cJSON_GetObjectItemCaseSensitive(rc, "radius_request");
    int n = 0;
    FwWatchArea areas[5];
    if (!cJSON_IsArray(list)) list = NULL;
    if (!cJSON_IsObject(one)) one = NULL;
    for (z = list ? list->child : one; z && n < 5; z = list ? z->next : NULL) {
        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(z, "latitude"), *lon = cJSON_GetObjectItemCaseSensitive(z, "longitude");
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(z, "radius_km");
        if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;
        areas[n].lat = (float)lat->valuedouble;
        areas[n].lon = (float)lon->valuedouble;
        areas[n].radius_km = cJSON_IsNumber(r) ? (float)r->valuedouble : 50.0f;
        ++n;
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_areas, areas, sizeof areas[0] * (size_t)n);
    s_area_count = n;
    xSemaphoreGive(s_lock);
}

static void configure(void){
    uint32_t rev = 0;
    char *doc = fw_store_settings(&rev);
    FwFeedDisplay d;
    FwCommunityInfo info = {0};
    FwFlightSource src = doc ? fw_feed_source(doc) : FW_SOURCE_VENDOR;
    char extra[224];
    bool shown, ok = false, switched;
    s_store_serial = fw_store_serial();
    s_follow = doc ? fw_feed_follow_app(doc) : true;
    shown = doc && fw_feed_display(doc, &d);
    read_areas(doc);
    if (shown && src == FW_SOURCE_COMMUNITY) ok = fw_community_configure(doc, &info);   /* this task only */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (shown && src == FW_SOURCE_VENDOR) ok = s_has_key && build_request(s_key, doc);
    switched = src != s_source;
    if (switched) {                                  /* never show one source's list as the other's */
        memset(&s_frame, 0, sizeof s_frame);
        ++s_serial;
        s_source = src;
    }
    if (shown) {
        memcpy(&s_display, &d, sizeof d);
        ++s_display_serial;
    }
    if (ok) {
        if (switched || s_state != FW_FEED_LIVE) s_state = FW_FEED_WAITING;
    } else {
        s_state = shown && src == FW_SOURCE_VENDOR && !s_has_key ? FW_FEED_OFF : FW_FEED_NO_CONFIG;
    }
    xSemaphoreGive(s_lock);
    snprintf(extra, sizeof extra,
             "\"ok\":%s,\"source\":\"%s\",\"revision\":%u,\"config_bytes\":%u,\"request_bytes\":%u,"
             "\"areas\":%d,\"tracked\":%d,\"skipped_geo\":%d,\"sync_tracked\":%s",
             ok ? "true" : "false", src == FW_SOURCE_COMMUNITY ? "community" : "vendor", (unsigned)rev,
             doc ? (unsigned)strlen(doc) : 0u, (unsigned)s_request_len, info.areas, info.tracked,
             info.skipped_geo, s_sync_flight[0] && src == FW_SOURCE_VENDOR ? "true" : "false");
    fw_mem_report(ok ? "feed_configured" : "feed_no_config", extra);
    if (doc) { memset(doc, 0, strlen(doc)); free(doc); }
}

/* The Sync leg in its window (SYNC.md "Live trip"): on the vendor feed it
 * is asked for as one more tracked flight, so the vendor's answer carries it
 * live and the trip card shows it. Entering or leaving the window rebuilds the
 * request (through configure). */
static void sync_tracking(void){
    char want[16] = "";
    FwSyncLeg leg;
    if (fw_sync_store_tracking((uint32_t)time(NULL), &leg)) snprintf(want, sizeof want, "%s%s", leg.al, leg.fn);
    if (!strcmp(want, s_sync_flight)) return;
    snprintf(s_sync_flight, sizeof s_sync_flight, "%s", want);
    s_store_serial = fw_store_serial() - 1;            /* configure again on this pass */
}

/* One community step; publishes the list only when it changed. */
static void community_tick(void){
    bool good = false;
    uint32_t wait = fw_community_step(&s_parse, &good);   /* the vendor's staging list is idle meanwhile */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_source == FW_SOURCE_COMMUNITY && memcmp(&s_parse, &s_frame, sizeof s_frame)) {
        memcpy(&s_frame, &s_parse, sizeof s_frame);
        ++s_serial;
    }
    xSemaphoreGive(s_lock);
    if (good) s_state = FW_FEED_LIVE;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));      /* a settings change wakes it early */
}

static void feed_task(void *arg){
    (void)arg;
    for (;;) {
        int status, stage;
        size_t bytes;
        uint32_t t0, wait;
        if (s_source == FW_SOURCE_VENDOR) sync_tracking();
        if (fw_store_serial() != s_store_serial) configure();
        if (!fw_wifi_link_wait_online(POLL_MS)) continue;
        /* changes made in the FlightWall app: by the vendor feed's hash while
         * it answers, else once per connection and when the dashboard is
         * opened (app_settings.h) */
        if (fw_app_settings_step(s_has_key ? s_key : NULL, s_follow,
                                 s_source == FW_SOURCE_VENDOR && s_state == FW_FEED_LIVE,
                                 fw_wifi_link_connections())) continue;
        if (s_state == FW_FEED_NO_CONFIG || s_state == FW_FEED_OFF) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS)); continue; }
        if (s_source == FW_SOURCE_COMMUNITY) { community_tick(); continue; }
        t0 = now_ms();
        stage = poll_once(&status, &bytes);
        ++s_polls;
        if (stage) { ++s_fails; ++s_streak; } else s_streak = 0;
        if (stage || s_polls <= 3 || s_polls % 30 == 0) {
            char extra[160];
            snprintf(extra, sizeof extra,
                     "\"n\":%u,\"stage\":%d,\"status\":%d,\"bytes\":%u,\"flights\":%d,\"tracked\":%s,\"overflow\":%d,"
                     "\"ms\":%u,\"fails\":%u",
                     s_polls, stage, status, (unsigned)bytes, stage ? -1 : s_parse.count,
                     !stage && s_parse.tracked ? "true" : "false", stage ? 0 : s_parse.overflow,
                     (unsigned)(now_ms() - t0), s_fails);
            fw_mem_report("feed_poll", extra);
        }
        if (!stage) {
            uint32_t t1 = now_ms();
            fix_step();                                    /* the progress bar's positions, between polls */
            t0 += now_ms() - t1;
        }
        wait = POLL_MS;
        if (s_streak >= 3) wait = s_streak >= 5 ? 60000 : s_streak == 4 ? 40000 : 20000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));   /* a settings change wakes it early */
    }
}

void fw_feed_start(void){
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_has_key = load_key(s_key, sizeof s_key);
    if (!s_has_key) {
        memset(s_key, 0, sizeof s_key);
        fw_mem_report("feed_no_key", NULL);           /* vendor source: FW_FEED_OFF, demo screens */
    }
    fw_app_settings_init();
    s_store_serial = fw_store_serial() - 1;              /* configure on the first pass */
    configure();
    /* 8 KiB (5.5 KiB used at most on the board): the community source runs TLS (about 4 KiB, measured by the
     * route probe), streaming JSON and route parsing on this task; re-reading
     * the settings with cJSON alone left 308 bytes of 4 KiB on the board. */
    xTaskCreate(feed_task, "fw_feed", 8192, NULL, 4, &s_task);
}
