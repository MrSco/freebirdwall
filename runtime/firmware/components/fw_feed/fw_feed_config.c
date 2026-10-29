/* The stored configuration (usercfg/json, from GET /configuration): the
 * request body stock sends (FUN_420057c0 -> FUN_4201325c) and the display
 * settings it applies (FUN_420189b4). FEED.md. */
#include "fw_feed.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#define ZONES_MAX 5
#define AIRLINES_MAX 32
#define TRACKED_MAX 8

static const cJSON *item(const cJSON *o, const char *k){ return cJSON_GetObjectItemCaseSensitive(o, k); }

static double num(const cJSON *o, const char *k, double dflt){
    const cJSON *v = item(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool flag(const cJSON *o, const char *k, bool dflt){
    const cJSON *v = item(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static const char *str(const cJSON *o, const char *k){
    const cJSON *v = item(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* min/max altitude and name, shared by both zone kinds */
static void zone_tail(cJSON *out, const cJSON *in){
    double lo = num(in, "min_altitude", -1), hi = num(in, "max_altitude", -1);
    const char *name = str(in, "name");
    if (lo >= 0) cJSON_AddNumberToObject(out, "min_altitude", (int32_t)lo);
    if (hi >= 0) cJSON_AddNumberToObject(out, "max_altitude", (int32_t)hi);
    if (name && *name) cJSON_AddStringToObject(out, "name", name);
}

/* FUN_42013160 */
static cJSON *radius_zone(const cJSON *in){
    cJSON *z = cJSON_CreateObject();
    cJSON_AddStringToObject(z, "type", "radius");
    cJSON_AddNumberToObject(z, "latitude", num(in, "latitude", 0));
    cJSON_AddNumberToObject(z, "longitude", num(in, "longitude", 0));
    cJSON_AddNumberToObject(z, "radius_km", num(in, "radius_km", 50.0));
    zone_tail(z, in);
    return z;
}

/* FUN_42013030: coordinate pairs passed through in config order */
static cJSON *geo_zone(const cJSON *in){
    cJSON *z = cJSON_CreateObject(), *coords;
    const cJSON *src = item(in, "coordinates"), *pair;
    cJSON_AddStringToObject(z, "type", "geo");
    coords = cJSON_AddArrayToObject(z, "coordinates");
    cJSON_ArrayForEach(pair, src) {
        const cJSON *a = cJSON_GetArrayItem(pair, 0), *b = cJSON_GetArrayItem(pair, 1);
        cJSON *out;
        if (!cJSON_IsArray(pair) || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
        out = cJSON_CreateArray();
        cJSON_AddItemToArray(out, cJSON_CreateNumber(a->valuedouble));
        cJSON_AddItemToArray(out, cJSON_CreateNumber(b->valuedouble));
        cJSON_AddItemToArray(coords, out);
    }
    zone_tail(z, in);
    return z;
}

/* Zones of one kind: the array (at most 5), or the singular alone, or the
 * legacy top-level form (FUN_42004598). Returns how many were added. */
static int zones(const cJSON *rc, const char *kind, const char *single, const char *plural, cJSON *list){
    const cJSON *arr = item(rc, plural), *one = item(rc, single), *z;
    bool radius = !strcmp(kind, "radius");
    int n = 0;
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        cJSON_ArrayForEach(z, arr) {
            if (n == ZONES_MAX) break;
            if (!cJSON_IsObject(z)) continue;
            cJSON_AddItemToArray(list, radius ? radius_zone(z) : geo_zone(z));
            ++n;
        }
    } else if (cJSON_IsObject(one)) {
        cJSON_AddItemToArray(list, radius ? radius_zone(one) : geo_zone(one));
        n = 1;
    } else {
        const char *type = str(rc, "type");
        if (type && !strcmp(type, kind)) {
            cJSON_AddItemToArray(list, radius ? radius_zone(rc) : geo_zone(rc));
            n = 1;
        }
    }
    return n;
}

/* FUN_42002eb8 / FUN_42003bdc */
static cJSON *filters(const cJSON *in){
    static const char *const shows[] = {"show_light_ac", "show_heli_ac", "show_commercial_ac", "show_smalljet_ac",
                                        "show_other_ac", "show_na_routes"};
    cJSON *f = cJSON_CreateObject(), *codes = cJSON_CreateArray();
    const cJSON *list = item(in, "airline_icao_filter"), *c;
    double lo = num(in, "min_speed_mps", -1), hi = num(in, "max_speed_mps", -1);
    int n = 0;
    for (size_t i = 0; i < sizeof shows / sizeof shows[0]; ++i) cJSON_AddBoolToObject(f, shows[i], flag(in, shows[i], true));
    if (lo >= 0) cJSON_AddNumberToObject(f, "min_speed_mps", (int32_t)lo);
    if (hi >= 0) cJSON_AddNumberToObject(f, "max_speed_mps", (int32_t)hi);
    cJSON_ArrayForEach(c, list) {
        char code[4];
        if (n == AIRLINES_MAX) break;
        if (!cJSON_IsString(c) || strlen(c->valuestring) != 3) continue;
        for (int i = 0; i < 3; ++i) {
            char ch = c->valuestring[i];
            code[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        }
        code[3] = 0;
        cJSON_AddItemToArray(codes, cJSON_CreateString(code));
        ++n;
    }
    if (n) cJSON_AddItemToObject(f, "airline_icao_filter", codes);
    else cJSON_Delete(codes);
    return f;
}

/* FUN_42003d24 */
static cJSON *tracked(const cJSON *in){
    cJSON *t = cJSON_CreateObject();
    const char *fn = str(in, "flight_number"), *at = str(in, "created_at");
    char buf[32];
    if (fn && *fn) { snprintf(buf, 12, "%s", fn); cJSON_AddStringToObject(t, "flight_number", buf); }
    if (at && *at) { snprintf(buf, 28, "%s", at); cJSON_AddStringToObject(t, "created_at", buf); }
    cJSON_AddBoolToObject(t, "auto_remove", flag(in, "auto_remove", true));
    cJSON_AddBoolToObject(t, "show_distance_travelled", flag(in, "show_distance_travelled", false));
    cJSON_AddBoolToObject(t, "show_metrics", flag(in, "show_metrics", false));
    return t;
}

/* A flight number as the app takes it: 2..11 of A-Z 0-9. */
static bool flight_number(const char *s){
    size_t n = s ? strlen(s) : 0;
    if (n < 2 || n > 11) return false;
    for (; *s; ++s) if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return false;
    return true;
}

static bool same_number(const char *a, const char *b){
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return false;
    }
    return !*a && !*b;
}

int fw_feed_body(const char *config_json, char *out, size_t cap){ return fw_feed_body_sync(config_json, NULL, out, cap); }

int fw_feed_body_sync(const char *config_json, const char *sync_flight, char *out, size_t cap){
    cJSON *root = cJSON_Parse(config_json), *body, *radius, *geo, *t;
    const cJSON *rc = item(root, "request_config"), *df, *tf, *e;
    char *text;
    int len = -1, nr, ng, nt = 0;
    if (!cJSON_IsObject(rc)) { cJSON_Delete(root); return -1; }
    radius = cJSON_CreateArray();
    geo = cJSON_CreateArray();
    nr = zones(rc, "radius", "radius_request", "radius_requests", radius);
    ng = zones(rc, "geo", "geo_request", "geo_requests", geo);
    body = cJSON_CreateObject();
    if (nr) cJSON_AddItemToObject(body, "radius_request", cJSON_Duplicate(cJSON_GetArrayItem(radius, 0), 1));
    if (ng) cJSON_AddItemToObject(body, "geo_request", cJSON_Duplicate(cJSON_GetArrayItem(geo, 0), 1));
    if (nr) cJSON_AddItemToObject(body, "radius_requests", radius); else cJSON_Delete(radius);
    if (ng) cJSON_AddItemToObject(body, "geo_requests", geo); else cJSON_Delete(geo);
    df = item(rc, "data_filters");
    if (cJSON_IsObject(df)) cJSON_AddItemToObject(body, "data_filters", filters(df));
    tf = item(rc, "tracked_flights");
    t = cJSON_CreateArray();
    cJSON_ArrayForEach(e, tf) {
        if (nt == TRACKED_MAX) break;
        if (!cJSON_IsObject(e)) continue;
        cJSON_AddItemToArray(t, tracked(e));
        ++nt;
    }
    if (flight_number(sync_flight)) {
        bool there = false;
        cJSON_ArrayForEach(e, t) {
            const char *fn = str(e, "flight_number");
            if (fn && same_number(fn, sync_flight)) there = true;
        }
        if (!there) {
            cJSON *leg = cJSON_CreateObject();
            cJSON_AddStringToObject(leg, "flight_number", sync_flight);
            cJSON_AddBoolToObject(leg, "auto_remove", false);
            cJSON_AddBoolToObject(leg, "show_distance_travelled", true);
            cJSON_AddBoolToObject(leg, "show_metrics", true);
            if (nt == TRACKED_MAX) cJSON_DeleteItemFromArray(t, nt - 1);   /* the leg in the air takes the last place */
            else ++nt;
            cJSON_AddItemToArray(t, leg);
        }
    }
    if (nt) cJSON_AddItemToObject(body, "tracked_flights", t); else cJSON_Delete(t);
    text = cJSON_PrintUnformatted(body);
    if (text && strlen(text) < cap) {
        len = (int)strlen(text);
        memcpy(out, text, (size_t)len + 1);
    }
    cJSON_free(text);
    cJSON_Delete(body);
    cJSON_Delete(root);
    return len;
}

/* ------------------------------------------------------------ display_config */

static void copy(char *dst, size_t cap, const char *s, const char *dflt){
    snprintf(dst, cap, "%s", s ? s : dflt);
}

FwFlightSource fw_feed_source(const char *config_json){
    cJSON *root = config_json ? cJSON_Parse(config_json) : NULL;
    const char *s = str(item(root, "owned_config"), "flight_source");
    FwFlightSource src = s && !strcmp(s, "community") ? FW_SOURCE_COMMUNITY : FW_SOURCE_VENDOR;
    cJSON_Delete(root);
    return src;
}

bool fw_feed_follow_app(const char *config_json){
    cJSON *root = config_json ? cJSON_Parse(config_json) : NULL;
    bool follow = !cJSON_IsFalse(item(item(root, "owned_config"), "follow_app"));
    cJSON_Delete(root);
    return follow;
}

/* A zone offset in minutes: the number under key in o, else dflt. */
static int16_t zone_offset(const cJSON *o, int16_t dflt, const char *key){
    const cJSON *v = item(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < -720 || v->valuedouble > 840) return dflt;
    return (int16_t)v->valuedouble;
}

bool fw_feed_display(const char *config_json, FwFeedDisplay *out){
    cJSON *root = cJSON_Parse(config_json);
    const cJSON *dc = item(root, "display_config"), *md, *tz, *clock, *anchors, *a;
    double v;
    memset(out, 0, sizeof *out);
    fw_units_parse(&out->units, NULL, NULL, NULL, NULL, NULL);
    copy(out->layout, sizeof out->layout, NULL, "DEFAULT");
    copy(out->loading, sizeof out->loading, NULL, "TEXT");
    copy(out->locale, sizeof out->locale, NULL, "en");
    out->dwell_ms = fw_display_speed_ms(NULL);
    out->max_flights = fw_max_flights_parse(NULL, false, false);
    out->border_percent = 100;
    out->display_enabled = true;
    {   /* owned: how fast long names scroll (FLIGHT-SCREENS.md "Long text") */
        const char *sp = str(item(root, "owned_config"), "scroll_speed");
        out->scroll_px_ms = sp && !strcmp(sp, "slow") ? 80 : sp && !strcmp(sp, "fast") ? 20 : 40;
        out->progress_bar = cJSON_IsTrue(item(item(root, "owned_config"), "progress_bar"));
        out->heartbeat_enabled = flag(item(root, "owned_config"), "heartbeat_enabled", true);
    }
    if (!cJSON_IsObject(dc)) { cJSON_Delete(root); return false; }
    out->has_display_config = true;
    md = item(dc, "metric_display");
    fw_units_parse(&out->units, str(md, "alt_b_unit"), str(md, "vel_unit"), str(md, "vr_unit"), str(md, "trk_unit"),
                   str(md, "dist_unit"));
    out->title = fw_title_parse(str(dc, "title_text"));
    copy(out->layout, sizeof out->layout, str(dc, "layout_template"), "DEFAULT");
    copy(out->loading, sizeof out->loading, str(dc, "loading_template"), "TEXT");
    copy(out->locale, sizeof out->locale, str(dc, "locale"), "en");
    out->dwell_ms = fw_display_speed_ms(str(dc, "flight_display_speed"));
    {
        const cJSON *hide = item(dc, "hide_max_flights_screen");
        out->max_flights = fw_max_flights_parse(str(dc, "max_flights_behavior"), cJSON_IsBool(hide), cJSON_IsTrue(hide));
    }
    out->border = flag(dc, "border_enable", false);
    v = num(dc, "border_brightness_percent", 100);
    out->border_percent = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    v = num(dc, "brightness_percent", 0);
    out->brightness_percent = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    anchors = item(dc, "anchor_airports");
    cJSON_ArrayForEach(a, anchors) {
        if (out->anchor_count == 8) break;
        if (cJSON_IsString(a) && *a->valuestring)
            copy(out->anchors[out->anchor_count++], sizeof out->anchors[0], a->valuestring, "");
    }
    tz = item(dc, "time_zone");
    /* time_zone, else the older clock_utc_offset_minutes, else no zone */
    out->utc_offset_min = zone_offset(tz, zone_offset(dc, FW_FEED_ZONE_UNSET, "clock_utc_offset_minutes"),
                                      "standard_utc_offset_minutes");
    copy(out->dst_rule, sizeof out->dst_rule, str(tz, "dst_rule"), "");
    out->clock_24h = flag(dc, "clock_is_24hr", false);
    out->clock_seconds = flag(dc, "clock_show_seconds", false);
    out->display_enabled = flag(dc, "display_enabled", true);
    {
        const cJSON *sl = item(dc, "sleep_time");
        const char *a = str(sl, "start"), *b = str(sl, "end");
        int ah, am, bh, bm;
        if (a && b && sscanf(a, "%d:%d", &ah, &am) == 2 && sscanf(b, "%d:%d", &bh, &bm) == 2 && ah >= 0 && ah < 24 &&
            am >= 0 && am < 60 && bh >= 0 && bh < 24 && bm >= 0 && bm < 60) {
            out->has_sleep = true;
            out->sleep_start_min = (int16_t)(ah * 60 + am);
            out->sleep_end_min = (int16_t)(bh * 60 + bm);
            v = num(sl, "sleep_brightness_percent", 0);
            out->sleep_percent = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        }
    }
    clock = item(dc, "clock");
    {
        /* FUN_420189b4: objects only, at most two; FUN_420182bc reads each
         * time_zone (missing parts: no zone, no DST). */
        const cJSON *e;
        cJSON_ArrayForEach(e, item(clock, "additional_timezones")) {
            const cJSON *z;
            if (out->extra_count == 2) break;
            if (!cJSON_IsObject(e)) continue;
            z = item(e, "time_zone");
            copy(out->extra[out->extra_count].label, sizeof out->extra[0].label, str(e, "label"), "");
            out->extra[out->extra_count].utc_offset_min = zone_offset(z, FW_FEED_ZONE_UNSET, "standard_utc_offset_minutes");
            copy(out->extra[out->extra_count].dst_rule, sizeof out->extra[0].dst_rule, str(z, "dst_rule"), "");
            ++out->extra_count;
        }
    }
    out->clock_large = str(clock, "style") && !strcmp(str(clock, "style"), "LARGE");
    out->clock_show_date = flag(clock, "show_date", false);
    {
        const cJSON *df = item(dc, "date_format");
        const char *order = str(df, "order");
        out->clock_day_first = order && !strcmp(order, "day_month");
        out->clock_weekday = flag(df, "show_weekday", false);
    }
    cJSON_Delete(root);
    return true;
}
