/* Bounded on-device flight analytics; see fw_analytics.h and ANALYTICS.md. */
#include "fw_analytics.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAYS 8
#define SETS (FW_AN_RECENT / WAYS)
#define MAGIC 0x4e415746u            /* "FWAN" little endian */
#define VERSION 2
#define TRACK_GAP_S 120u             /* max seconds between consecutive fixes to draw a path segment */
#define HEAT_CEIL 240u               /* halve grid and double scale when any cell hits this */
#define PI_D 3.14159265358979323846

/* Curated global rarity table by ICAO aircraft type designator (sorted by
 * code for binary search, kept in flash RODATA -> 0 bytes of RAM).
 *   3 = One of a kind / ultra-rare (~1-15 active worldwide)
 *   2 = Very rare (~15-100 active worldwide, iconic heavies/warbirds/military)
 *   1 = Notable (uncommon widebodies, retro/classic airliners, specialty jets) */
typedef struct { char code[5]; uint8_t tier; } RareEntry;
static const RareEntry RARE_TABLE[] = {
    {"A10",  2}, {"A124", 3}, {"A140", 3}, {"A148", 3}, {"A158", 3},
    {"A225", 3}, {"A306", 2}, {"A30B", 3}, {"A310", 3}, {"A318", 2},
    {"A337", 3}, {"A338", 2}, {"A342", 3}, {"A343", 2}, {"A345", 3},
    {"A346", 2}, {"A35K", 1}, {"A388", 2}, {"A3ST", 3}, {"A400", 2},
    {"AN12", 2}, {"AN22", 3}, {"AN24", 2}, {"AN26", 2}, {"AN32", 2},
    {"AN72", 3}, {"AT43", 1}, {"AT72", 1}, {"AVRO", 2},
    {"B1",   3}, {"B17",  3}, {"B2",   3}, {"B24",  3}, {"B25",  3},
    {"B29",  3}, {"B350", 1}, {"B39M", 1}, {"B461", 2}, {"B462", 2},
    {"B463", 2}, {"B52",  3}, {"B703", 3}, {"B712", 1}, {"B721", 3},
    {"B722", 3}, {"B732", 3}, {"B733", 2}, {"B734", 1}, {"B735", 2},
    {"B736", 2}, {"B742", 3}, {"B743", 3}, {"B744", 2}, {"B748", 2},
    {"B74L", 3}, {"B74S", 3}, {"B752", 1}, {"B753", 2}, {"B762", 1},
    {"B764", 1}, {"B77L", 1}, {"B78X", 1}, {"BA11", 3}, {"BCS1", 1},
    {"BLCF", 3},
    {"C130", 2}, {"C135", 2}, {"C141", 3}, {"C17",  2}, {"C2",   3},
    {"C27J", 2}, {"C30J", 2}, {"C46",  3}, {"C47",  3}, {"C5",   3},
    {"C5M",  3}, {"C919", 2}, {"CL2T", 2}, {"CL41", 2}, {"CONC", 3},
    {"CONI", 3}, {"CRJ1", 2}, {"CVLT", 3},
    {"DC10", 3}, {"DC3",  3}, {"DC6",  3}, {"DC8",  3}, {"DC87", 3},
    {"DC9",  3}, {"DC93", 3}, {"DH8A", 2}, {"DH8B", 1}, {"DHC2", 2},
    {"DHC3", 2}, {"DHC6", 1}, {"DHC7", 3},
    {"E120", 2}, {"E135", 1}, {"E2",   3}, {"E290", 1}, {"E295", 1},
    {"E3CF", 3}, {"E3TF", 3}, {"E4",   3}, {"E6",   3}, {"EUFI", 2},
    {"F100", 2}, {"F14",  3}, {"F15",  2}, {"F16",  2}, {"F18",  2},
    {"F18H", 2}, {"F18S", 2}, {"F22",  3}, {"F27",  3}, {"F28",  3},
    {"F35",  2}, {"F4",   3}, {"F4U",  3}, {"F50",  2}, {"F70",  2},
    {"FA10", 1}, {"FA8X", 1}, {"FORD", 3},
    {"G222", 3}, {"G700", 2}, {"G800", 3}, {"GL7T", 1}, {"GLF1", 3},
    {"GLF2", 3}, {"GLF3", 2},
    {"H47",  2}, {"HAWK", 2}, {"HDJT", 1},
    {"IL62", 3}, {"IL76", 2}, {"IL86", 3}, {"IL96", 3},
    {"J328", 2}, {"JS41", 2}, {"JU52", 3},
    {"K35R", 2}, {"KC10", 3}, {"KC46", 2},
    {"L101", 3}, {"L188", 3}, {"L39",  2},
    {"MD11", 2}, {"MD82", 2}, {"MD83", 2}, {"MD87", 2}, {"MD88", 2},
    {"MD90", 3}, {"MIG2", 3}, {"MU2",  2},
    {"P180", 2}, {"P3",   2}, {"P38",  3}, {"P51",  3}, {"P8",   2},
    {"PC24", 1}, {"Q4",   3},
    {"R135", 3}, {"RFAL", 2}, {"RJ1H", 2}, {"RJ85", 2}, {"RQ4",  3},
    {"S64",  3}, {"SB20", 2}, {"SF34", 1}, {"SJ30", 2}, {"SPIT", 3},
    {"SR71", 3}, {"SU27", 3}, {"SU95", 2},
    {"T38",  2}, {"TORN", 2}, {"TU15", 3}, {"TU16", 3}, {"TU95", 3},
    {"U2",   3}, {"V22",  3}, {"VC25", 3}, {"WB57", 3},
};

static char upc(char c){ return (char)(c >= 'a' && c <= 'z' ? c - 32 : c); }

uint8_t fw_an_rarity(const char *ac_type){
    char key[5];
    size_t n = 0, lo = 0, hi = sizeof RARE_TABLE / sizeof RARE_TABLE[0];
    if (!ac_type || !*ac_type) return FW_AN_RARE_NONE;
    for (; ac_type[n] && n < 4; ++n) {
        char c = upc(ac_type[n]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return FW_AN_RARE_NONE;
        key[n] = c;
    }
    if (ac_type[n] != 0 || n < 2) return FW_AN_RARE_NONE;
    key[n] = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(RARE_TABLE[mid].code, key);
        if (cmp == 0) return RARE_TABLE[mid].tier;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return FW_AN_RARE_NONE;
}

const char *fw_an_rarity_label(uint8_t tier){
    switch (tier) {
    case FW_AN_RARE_ONE_OF_KIND: return "One of a kind";
    case FW_AN_RARE_VERY: return "Very rare";
    case FW_AN_RARE_NOTABLE: return "Notable";
    default: return "";
    }
}

void fw_an_init(FwAnalytics *a){
    memset(a, 0, sizeof *a);
    for (int i = 0; i < FW_AN_HOURS; ++i) a->hours[i].hour = -1;
    for (int i = 0; i < FW_AN_DAYS; ++i) a->days[i].day = -1;
    a->heat.scale = 1;
}

static uint32_t fnv(uint32_t h, const char *s){
    for (; s && *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

uint32_t fw_an_flight_key(const char *hex, const char *callsign, const char *reg){
    uint32_t h = 2166136261u;
    if (hex && *hex) h = fnv(fnv(h, "x:"), hex);
    else if (callsign && *callsign) h = fnv(fnv(h, "c:"), callsign);
    else if (reg && *reg) h = fnv(fnv(h, "r:"), reg);
    else return 0;
    return h ? h : 1;
}

static int64_t local_seconds(uint32_t now, int offset_min){ return (int64_t)now + (int64_t)offset_min * 60; }

static int32_t floor_div(int64_t v, int64_t d){ return (int32_t)(v >= 0 ? v / d : -((-v + d - 1) / d)); }

int32_t fw_an_local_hour(uint32_t now, int offset_min){ return floor_div(local_seconds(now, offset_min), 3600); }
int32_t fw_an_local_day(uint32_t now, int offset_min){ return floor_div(local_seconds(now, offset_min), 86400); }

static int ring(int32_t v, int n){ int r = (int)(v % n); return r < 0 ? r + n : r; }

static FwAnHour *hour_slot(FwAnalytics *a, int32_t hour){
    FwAnHour *h = &a->hours[ring(hour, FW_AN_HOURS)];
    if (h->hour == hour) return h;
    if (h->hour > hour) return NULL;
    memset(h, 0, sizeof *h);
    h->hour = hour;
    return h;
}

static FwAnDay *day_slot(FwAnalytics *a, int32_t day){
    FwAnDay *d = &a->days[ring(day, FW_AN_DAYS)];
    if (d->day == day) return d;
    if (d->day > day) return NULL;
    memset(d, 0, sizeof *d);
    d->day = day;
    return d;
}

const FwAnHour *fw_an_hour(const FwAnalytics *a, int32_t hour){
    const FwAnHour *h = &a->hours[ring(hour, FW_AN_HOURS)];
    return h->hour == hour ? h : NULL;
}

const FwAnDay *fw_an_day(const FwAnalytics *a, int32_t day){
    const FwAnDay *d = &a->days[ring(day, FW_AN_DAYS)];
    return d->day == day ? d : NULL;
}

/* Space-saving top-K for 3-letter airlines. */
static void count_airline(FwAnDay *d, const char *airline){
    char code[FW_AN_CODE];
    int i, empty = -1, min = -1;
    size_t n = 0;
    for (; airline[n] && n < FW_AN_CODE - 1; ++n) code[n] = upc(airline[n]);
    code[n] = 0;
    if (!n) return;
    for (i = 0; i < FW_AN_TOP; ++i) {
        FwAnAirline *e = &d->top[i];
        if (!e->code[0]) { if (empty < 0) empty = i; continue; }
        if (strcmp(e->code, code) == 0) {
            if (e->count < UINT16_MAX) ++e->count;
            return;
        }
        if (min < 0 || e->count < d->top[min].count) min = i;
    }
    if (empty >= 0) {
        memcpy(d->top[empty].code, code, sizeof code);
        d->top[empty].count = 1;
        d->top[empty].error = 0;
        return;
    }
    d->top[min].error = d->top[min].count;
    memcpy(d->top[min].code, code, sizeof code);
    if (d->top[min].count < UINT16_MAX) ++d->top[min].count;
}

/* Space-saving top-K for 2..4-char uppercase alphanumeric tags (aircraft types,
 * airport codes). Stored NUL-padded in 4 bytes. */
static bool clean_tag(const char *in, char out[4]){
    size_t n = 0;
    memset(out, 0, 4);
    if (!in) return false;
    for (; in[n] && n < 4; ++n) {
        char c = upc(in[n]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        out[n] = c;
    }
    return n >= 2 && in[n] == 0;
}

static void count_tag(FwAnTag *list, int cap, const char *raw){
    char code[4];
    int i, empty = -1, min = -1;
    if (!clean_tag(raw, code)) return;
    for (i = 0; i < cap; ++i) {
        FwAnTag *e = &list[i];
        if (!e->code[0]) { if (empty < 0) empty = i; continue; }
        if (memcmp(e->code, code, 4) == 0) {
            if (e->count < UINT16_MAX) ++e->count;
            return;
        }
        if (min < 0 || e->count < list[min].count) min = i;
    }
    if (empty >= 0) {
        memcpy(list[empty].code, code, 4);
        list[empty].count = 1;
        list[empty].error = 0;
        return;
    }
    list[min].error = list[min].count;
    memcpy(list[min].code, code, 4);
    if (list[min].count < UINT16_MAX) ++list[min].count;
}

static bool route_bit_get(const FwAnalytics *a, int idx){ return (a->route_bits[idx >> 3] >> (idx & 7)) & 1u; }
static void route_bit_set(FwAnalytics *a, int idx, bool v){
    if (v) a->route_bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    else a->route_bits[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

/* Deduplication: set-associative, least recently seen way replaced.
 * *slot_out receives the index in a->recent (0..FW_AN_RECENT-1). */
static bool new_sighting(FwAnalytics *a, uint32_t key, uint32_t now, int *slot_out){
    int base = (int)((key % SETS) * WAYS);
    FwAnRecent *set = &a->recent[base];
    int victim = 0;
    for (int i = 0; i < WAYS; ++i) {
        if (set[i].key == key) {
            bool fresh = now - set[i].last_seen >= FW_AN_REVISIT_S;
            if (now > set[i].last_seen) set[i].last_seen = now;
            if (fresh) route_bit_set(a, base + i, false);
            if (slot_out) *slot_out = base + i;
            return fresh;
        }
        if (!set[i].key) { victim = i; break; }
        if (set[i].last_seen < set[victim].last_seen) victim = i;
    }
    set[victim].key = key;
    set[victim].last_seen = now;
    route_bit_set(a, base + victim, false);
    if (slot_out) *slot_out = base + victim;
    return true;
}

static int find_recent(const FwAnalytics *a, uint32_t key, uint32_t now){
    int base = (int)((key % SETS) * WAYS);
    const FwAnRecent *set = &a->recent[base];
    for (int i = 0; i < WAYS; ++i)
        if (set[i].key == key && now - set[i].last_seen < FW_AN_REVISIT_S) return base + i;
    return -1;
}

static void copy_upper(char *dst, size_t cap, const char *src){
    size_t n = 0;
    if (!cap) return;
    if (src) {
        for (; src[n] && n + 1 < cap; ++n) dst[n] = upc(src[n]);
    }
    dst[n] = 0;
}

static void record_rare(FwAnalytics *a, const FwAnObs *obs, uint8_t tier, uint32_t now){
    FwAnRare *r = &a->rare[a->rare_head];
    memset(r, 0, sizeof *r);
    r->seen_at = now;
    r->key = obs->key;
    r->tier = tier;
    copy_upper(r->ac_type, sizeof r->ac_type, obs->ac_type);
    copy_upper(r->cs, sizeof r->cs, obs->callsign);
    copy_upper(r->reg, sizeof r->reg, obs->reg);
    copy_upper(r->al, sizeof r->al, obs->airline);
    copy_upper(r->orig, sizeof r->orig, obs->orig);
    copy_upper(r->dest, sizeof r->dest, obs->dest);
    a->rare_head = (uint8_t)((a->rare_head + 1) % FW_AN_RARE);
    if (a->rare_count < FW_AN_RARE) ++a->rare_count;
    if (a->rare_total < UINT16_MAX) ++a->rare_total;
}

/* ------------------------------------------------------------ flight-path heatmap */

static void heat_reset(FwAnalytics *a){
    memset(a->heat.cells, 0, sizeof a->heat.cells);
    memset(a->tracks, 0, sizeof a->tracks);
    a->heat.scale = 1;
    a->heat.since = 0;
    a->heat.points = 0;
}

void fw_an_set_bounds(FwAnalytics *a, double lat_min, double lat_max, double lon_min, double lon_max){
    double clat, clon, dlat, dlon;
    if (lat_max < lat_min) { double t = lat_min; lat_min = lat_max; lat_max = t; }
    if (lon_max < lon_min) { double t = lon_min; lon_min = lon_max; lon_max = t; }
    clat = 0.5 * (lat_min + lat_max);
    clon = 0.5 * (lon_min + lon_max);
    dlat = 0.5 * (lat_max - lat_min);
    dlon = 0.5 * (lon_max - lon_min);
    if (dlat < 0.01) dlat = 0.01;
    if (dlon < 0.01) dlon = 0.01;
    if (dlat > 5.0) dlat = 5.0;
    if (dlon > 8.0) dlon = 8.0;
    if (a->heat.dlat > 0.0f && a->heat.dlon > 0.0f) {
        if (fabs(clat - a->heat.lat) > a->heat.dlat * 0.2 || fabs(clon - a->heat.lon) > a->heat.dlon * 0.2 ||
            fabs(dlat - a->heat.dlat) > a->heat.dlat * 0.2 || fabs(dlon - a->heat.dlon) > a->heat.dlon * 0.2)
            heat_reset(a);
    } else {
        heat_reset(a);
    }
    a->heat.lat = (float)clat;
    a->heat.lon = (float)clon;
    a->heat.dlat = (float)dlat;
    a->heat.dlon = (float)dlon;
    if (!a->heat.scale) a->heat.scale = 1;
}

void fw_an_set_area(FwAnalytics *a, double lat, double lon, double radius_km){
    double coslat, dlat, dlon;
    if (radius_km < 1.0) radius_km = 1.0;
    if (radius_km > 250.0) radius_km = 250.0;
    coslat = cos(lat * (PI_D / 180.0));
    if (coslat < 0.05) coslat = 0.05;
    dlat = (radius_km * 1.1) / 111.32;
    dlon = (radius_km * 1.1) / (111.32 * coslat);
    fw_an_set_bounds(a, lat - dlat, lat + dlat, lon - dlon, lon + dlon);
}

static void heat_hit(FwAnalytics *a, int x, int y){
    int idx;
    if (x < 0 || x >= FW_AN_GRID || y < 0 || y >= FW_AN_GRID) return;
    idx = y * FW_AN_GRID + x;
    if (a->heat.cells[idx] < 255) ++a->heat.cells[idx];
    if (a->heat.cells[idx] >= HEAT_CEIL && a->heat.scale < 32768u) {
        for (int i = 0; i < FW_AN_GRID * FW_AN_GRID; ++i)
            a->heat.cells[i] = (uint8_t)((a->heat.cells[i] + 1u) >> 1);
        a->heat.scale = (uint16_t)(a->heat.scale * 2u);
    }
}

/* Bresenham walk from (x0, y0) to (x1, y1), plotting every cell except the
 * starting cell (already plotted by the previous fix). */
static void heat_line(FwAnalytics *a, int x0, int y0, int x1, int y1){
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, steps = 0;
    while ((x0 != x1 || y0 != y1) && steps++ < FW_AN_GRID * 2) {
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        heat_hit(a, x0, y0);
    }
}

void fw_an_position(FwAnalytics *a, uint32_t key, double lat, double lon, uint32_t now){
    double dlat, dlon, fx, fy;
    int gx, gy, slot = -1, oldest = 0;
    if (!key || a->heat.dlat <= 0.0f || a->heat.dlon <= 0.0f || lat != lat || lon != lon) return;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
    dlat = (double)a->heat.dlat;
    dlon = (double)a->heat.dlon;
    fx = (lon - (double)a->heat.lon + dlon) / (2.0 * dlon) * FW_AN_GRID;
    fy = ((double)a->heat.lat + dlat - lat) / (2.0 * dlat) * FW_AN_GRID;   /* y=0 is north */
    if (fx < 0.0 || fx >= FW_AN_GRID || fy < 0.0 || fy >= FW_AN_GRID) return;
    gx = (int)fx;
    gy = (int)fy;
    if (!a->heat.since || now < a->heat.since) a->heat.since = now;
    if (a->heat.points < UINT16_MAX) ++a->heat.points;

    for (int i = 0; i < FW_AN_TRACKS; ++i) {
        if (a->tracks[i].used && a->tracks[i].key == key) { slot = i; break; }
        if (!a->tracks[i].used && slot < 0) slot = i;
        if (a->tracks[i].at < a->tracks[oldest].at) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (a->tracks[slot].used && a->tracks[slot].key == key && now >= a->tracks[slot].at &&
        now - a->tracks[slot].at <= TRACK_GAP_S) {
        if (gx != a->tracks[slot].x || gy != a->tracks[slot].y)
            heat_line(a, a->tracks[slot].x, a->tracks[slot].y, gx, gy);
    } else {
        heat_hit(a, gx, gy);
    }
    a->tracks[slot].key = key;
    a->tracks[slot].at = now;
    a->tracks[slot].x = (int8_t)gx;
    a->tracks[slot].y = (int8_t)gy;
    a->tracks[slot].used = 1;
}

/* ------------------------------------------------------------ observations */

static bool valid_airport(const char *s){
    char tmp[4];
    return clean_tag(s, tmp);
}

bool fw_an_observe_ex(FwAnalytics *a, const FwAnObs *obs, uint32_t now, int offset_min){
    FwAnHour *h;
    FwAnDay *d;
    int slot = -1;
    ++a->total_observations;
    if (!obs || !obs->key) return false;
    if (obs->has_pos) fw_an_position(a, obs->key, obs->lat, obs->lon, now);
    if (!new_sighting(a, obs->key, now, &slot)) return false;
    ++a->total_sightings;
    if ((h = hour_slot(a, fw_an_local_hour(now, offset_min))) && h->sightings < UINT16_MAX) ++h->sightings;
    if ((d = day_slot(a, fw_an_local_day(now, offset_min)))) {
        bool has_orig = valid_airport(obs->orig);
        bool has_dest = valid_airport(obs->dest);
        if (d->sightings < UINT16_MAX) ++d->sightings;
        if (obs->airline) count_airline(d, obs->airline);
        if (obs->ac_type) count_tag(d->types, FW_AN_DIM_TOP, obs->ac_type);
        if (has_orig || has_dest) {
            if (d->routes_known < UINT16_MAX) ++d->routes_known;
            if (has_orig) count_tag(d->orig, FW_AN_DIM_TOP, obs->orig);
            if (has_dest) count_tag(d->dest, FW_AN_DIM_TOP, obs->dest);
            if (slot >= 0) route_bit_set(a, slot, true);
        }
    }
    {
        uint8_t tier = fw_an_rarity(obs->ac_type);
        if (tier > FW_AN_RARE_NONE) record_rare(a, obs, tier, now);
    }
    return true;
}

bool fw_an_observe(FwAnalytics *a, uint32_t key, const char *airline, uint32_t now, int offset_min){
    FwAnObs o;
    memset(&o, 0, sizeof o);
    o.key = key;
    o.airline = airline;
    return fw_an_observe_ex(a, &o, now, offset_min);
}

void fw_an_route(FwAnalytics *a, uint32_t key, const char *orig, const char *dest, uint32_t now, int offset_min){
    bool has_orig = valid_airport(orig), has_dest = valid_airport(dest);
    int slot;
    FwAnDay *d;
    if (!key || (!has_orig && !has_dest)) return;
    slot = find_recent(a, key, now);
    if (slot < 0 || route_bit_get(a, slot)) return;
    route_bit_set(a, slot, true);
    if ((d = day_slot(a, fw_an_local_day(now, offset_min)))) {
        if (d->routes_known < UINT16_MAX) ++d->routes_known;
        if (has_orig) count_tag(d->orig, FW_AN_DIM_TOP, orig);
        if (has_dest) count_tag(d->dest, FW_AN_DIM_TOP, dest);
    }
    /* Enrich matching recent rare sighting if its route was still empty. */
    for (int i = 0; i < (int)a->rare_count; ++i) {
        FwAnRare *r = &a->rare[i];
        if (r->key == key && now >= r->seen_at && now - r->seen_at < FW_AN_REVISIT_S) {
            if (!r->orig[0] && has_orig) copy_upper(r->orig, sizeof r->orig, orig);
            if (!r->dest[0] && has_dest) copy_upper(r->dest, sizeof r->dest, dest);
        }
    }
}

void fw_an_covered(FwAnalytics *a, uint32_t now, int offset_min){
    int64_t ls = local_seconds(now, offset_min);
    int minute = ring(floor_div(ls, 60), 60);
    FwAnHour *h = hour_slot(a, fw_an_local_hour(now, offset_min));
    FwAnDay *d;
    if (!h || (h->minute_bits >> minute & 1u)) return;
    h->minute_bits |= (uint64_t)1 << minute;
    ++h->minutes_observed;
    if ((d = day_slot(a, fw_an_local_day(now, offset_min))) && d->minutes_observed < 1440) ++d->minutes_observed;
}

int fw_an_top_airlines(const FwAnalytics *a, int32_t day, FwAnAirline out[FW_AN_TOP]){
    const FwAnDay *d = fw_an_day(a, day);
    int n = 0;
    if (!d) return 0;
    for (int i = 0; i < FW_AN_TOP; ++i)
        if (d->top[i].code[0]) out[n++] = d->top[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0; --j) {
            FwAnAirline *x = &out[j - 1], *y = &out[j], t;
            if (x->count > y->count || (x->count == y->count && strcmp(x->code, y->code) <= 0)) break;
            t = *x; *x = *y; *y = t;
        }
    return n;
}

static int top_tags(const FwAnTag *src, FwAnTag out[FW_AN_DIM_TOP]){
    int n = 0;
    for (int i = 0; i < FW_AN_DIM_TOP; ++i)
        if (src[i].code[0]) out[n++] = src[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0; --j) {
            FwAnTag *x = &out[j - 1], *y = &out[j], t;
            if (x->count > y->count || (x->count == y->count && memcmp(x->code, y->code, 4) <= 0)) break;
            t = *x; *x = *y; *y = t;
        }
    return n;
}

int fw_an_top_types(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]){
    const FwAnDay *d = fw_an_day(a, day);
    return d ? top_tags(d->types, out) : 0;
}

int fw_an_top_origins(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]){
    const FwAnDay *d = fw_an_day(a, day);
    return d ? top_tags(d->orig, out) : 0;
}

int fw_an_top_dests(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]){
    const FwAnDay *d = fw_an_day(a, day);
    return d ? top_tags(d->dest, out) : 0;
}

/* ------------------------------------------------------------ streaming JSON export */

typedef struct {
    FwAnEmit emit;
    void *ctx;
    char buf[512];
    size_t len;
} Stream;

static void sflush(Stream *s){
    if (s->len) {
        s->emit(s->ctx, s->buf, s->len);
        s->len = 0;
    }
}

static void sput(Stream *s, const char *p, size_t n){
    while (n) {
        size_t room = sizeof s->buf - s->len;
        size_t take = n < room ? n : room;
        memcpy(s->buf + s->len, p, take);
        s->len += take;
        p += take;
        n -= take;
        if (s->len == sizeof s->buf) sflush(s);
    }
}

static void sf(Stream *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sf(Stream *s, const char *fmt, ...){
    char tmp[192];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sput(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void sstr(Stream *s, const char *v){
    sput(s, "\"", 1);
    for (; v && *v; ++v) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; sput(s, e, 2); }
        else if (c < 0x20 || c >= 0x7f) continue;
        else sput(s, (const char *)&c, 1);
    }
    sput(s, "\"", 1);
}

static void stags(Stream *s, const FwAnTag *tags, int n){
    sput(s, "[", 1);
    for (int i = 0; i < n; ++i) {
        char code[5];
        memcpy(code, tags[i].code, 4);
        code[4] = 0;
        sf(s, "%s[\"%s\",%u]", i ? "," : "", code, (unsigned)tags[i].count);
    }
    sput(s, "]", 1);
}

void fw_an_json_stream(const FwAnalytics *a, FwAnEmit emit, void *ctx){
    Stream s = {emit, ctx, {0}, 0};
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    bool first = true;

    sf(&s, "{\"hours\":[");
    for (int i = 0; i < FW_AN_HOURS; ++i)
        if (a->hours[i].hour >= 0) {
            if (a->hours[i].hour < lo) lo = a->hours[i].hour;
            if (a->hours[i].hour > hi) hi = a->hours[i].hour;
        }
    for (int32_t h = lo; lo <= hi && h <= hi; ++h) {
        const FwAnHour *x = fw_an_hour(a, h);
        if (!x) continue;
        sf(&s, "%s[%ld,%u,%u]", first ? "" : ",", (long)h, (unsigned)x->sightings, (unsigned)x->minutes_observed);
        first = false;
    }

    sf(&s, "],\"days\":[");
    lo = INT32_MAX; hi = INT32_MIN; first = true;
    for (int i = 0; i < FW_AN_DAYS; ++i)
        if (a->days[i].day >= 0) {
            if (a->days[i].day < lo) lo = a->days[i].day;
            if (a->days[i].day > hi) hi = a->days[i].day;
        }
    for (int32_t d = lo; lo <= hi && d <= hi; ++d) {
        const FwAnDay *x = fw_an_day(a, d);
        FwAnAirline top[FW_AN_TOP];
        FwAnTag types[FW_AN_DIM_TOP], orig[FW_AN_DIM_TOP], dest[FW_AN_DIM_TOP];
        int na, nt, no, nd;
        if (!x) continue;
        na = fw_an_top_airlines(a, d, top);
        nt = top_tags(x->types, types);
        no = top_tags(x->orig, orig);
        nd = top_tags(x->dest, dest);
        sf(&s, "%s[%ld,%u,%u,[", first ? "" : ",", (long)d, (unsigned)x->sightings, (unsigned)x->minutes_observed);
        for (int i = 0; i < na; ++i) sf(&s, "%s[\"%s\",%u]", i ? "," : "", top[i].code, (unsigned)top[i].count);
        sf(&s, "],%u,", (unsigned)x->routes_known);
        stags(&s, types, nt);
        sput(&s, ",", 1);
        stags(&s, orig, no);
        sput(&s, ",", 1);
        stags(&s, dest, nd);
        sput(&s, "]", 1);
        first = false;
    }

    /* Recent rare sightings, newest first. */
    sf(&s, "],\"rare\":[");
    first = true;
    for (int k = 0; k < (int)a->rare_count; ++k) {
        int idx = ((int)a->rare_head - 1 - k + FW_AN_RARE) % FW_AN_RARE;
        const FwAnRare *r = &a->rare[idx];
        if (!r->tier) continue;
        sf(&s, "%s[%u,%u,", first ? "" : ",", (unsigned)r->seen_at, (unsigned)r->tier);
        sstr(&s, r->ac_type); sput(&s, ",", 1);
        sstr(&s, r->cs); sput(&s, ",", 1);
        sstr(&s, r->reg); sput(&s, ",", 1);
        sstr(&s, r->al); sput(&s, ",", 1);
        sstr(&s, r->orig); sput(&s, ",", 1);
        sstr(&s, r->dest); sput(&s, "]", 1);
        first = false;
    }
    sf(&s, "],\"rare_total\":%u", (unsigned)a->rare_total);

    /* Flight-path heatmap: sparse [cell_idx, value] pairs. */
    if (a->heat.dlat > 0.0f && a->heat.dlon > 0.0f) {
        sf(&s, ",\"heat\":{\"lat\":%.5f,\"lon\":%.5f,\"dlat\":%.5f,\"dlon\":%.5f,\"n\":%d,\"scale\":%u,\"since\":%u,\"pts\":%u,\"cells\":[",
           (double)a->heat.lat, (double)a->heat.lon, (double)a->heat.dlat, (double)a->heat.dlon, FW_AN_GRID,
           (unsigned)(a->heat.scale ? a->heat.scale : 1), (unsigned)a->heat.since, (unsigned)a->heat.points);
        first = true;
        for (int i = 0; i < FW_AN_GRID * FW_AN_GRID; ++i) {
            if (!a->heat.cells[i]) continue;
            sf(&s, "%s[%d,%u]", first ? "" : ",", i, (unsigned)a->heat.cells[i]);
            first = false;
        }
        sf(&s, "]}");
    } else {
        sf(&s, ",\"heat\":null");
    }

    sput(&s, "}", 1);
    sflush(&s);
}

typedef struct { char *buf; size_t cap, len; } BufOut;
static void buf_emit(void *ctx, const char *data, size_t len){
    BufOut *o = (BufOut *)ctx;
    if (o->buf && o->len + 1 < o->cap) {
        size_t room = o->cap - 1 - o->len;
        size_t take = len < room ? len : room;
        memcpy(o->buf + o->len, data, take);
    }
    o->len += len;
}

size_t fw_an_json(const FwAnalytics *a, char *buf, size_t cap){
    BufOut o = {buf, cap, 0};
    if (buf && cap) buf[0] = 0;
    fw_an_json_stream(a, buf_emit, &o);
    if (buf && cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return o.len;
}

/* ------------------------------------------------------------ checkpoint v2 */

static uint32_t crc32(const uint8_t *p, size_t n){
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

typedef struct { uint8_t *p; size_t cap, len; bool ok; } W;
static void w8(W *w, uint32_t v){ if (w->len < w->cap) w->p[w->len] = (uint8_t)v; else w->ok = false; ++w->len; }
static void w16(W *w, uint32_t v){ w8(w, v); w8(w, v >> 8); }
static void w32(W *w, uint32_t v){ w16(w, v); w16(w, v >> 16); }
static void w64(W *w, uint64_t v){ w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static void wf32(W *w, float f){ uint32_t u; memcpy(&u, &f, 4); w32(w, u); }
static void wbytes(W *w, const void *src, size_t n){
    const uint8_t *b = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i) w8(w, b[i]);
}
static void wtags(W *w, const FwAnTag *tags, int n){
    for (int i = 0; i < n; ++i) {
        wbytes(w, tags[i].code, 4);
        w16(w, tags[i].count);
        w16(w, tags[i].error);
    }
}

typedef struct { const uint8_t *p; size_t len, pos; bool ok; } R;
static uint32_t r8(R *r){ if (r->pos >= r->len) { r->ok = false; return 0; } return r->p[r->pos++]; }
static uint32_t r16(R *r){ uint32_t v = r8(r); return v | r8(r) << 8; }
static uint32_t r32(R *r){ uint32_t v = r16(r); return v | r16(r) << 16; }
static uint64_t r64(R *r){ uint64_t v = r32(r); return v | (uint64_t)r32(r) << 32; }
static float rf32(R *r){ uint32_t u = r32(r); float f; memcpy(&f, &u, 4); return f; }
static void rbytes(R *r, void *dst, size_t n){
    uint8_t *b = (uint8_t *)dst;
    for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)r8(r);
}
static void rtags(R *r, FwAnTag *tags, int n){
    for (int i = 0; i < n; ++i) {
        rbytes(r, tags[i].code, 4);
        tags[i].count = (uint16_t)r16(r);
        tags[i].error = (uint16_t)r16(r);
    }
}

size_t fw_an_save(const FwAnalytics *a, uint8_t *buf, size_t cap){
    W w = {buf, cap, 0, true};
    uint16_t nonzero = 0;
    w32(&w, MAGIC);
    w16(&w, VERSION);
    w8(&w, FW_AN_HOURS);
    w8(&w, FW_AN_DAYS);
    w8(&w, FW_AN_TOP);
    w8(&w, FW_AN_DIM_TOP);
    w8(&w, FW_AN_RARE);
    w8(&w, FW_AN_GRID);
    w32(&w, a->total_sightings);
    w32(&w, a->total_observations);
    for (int i = 0; i < FW_AN_HOURS; ++i) {
        const FwAnHour *h = &a->hours[i];
        w32(&w, (uint32_t)h->hour);
        w16(&w, h->sightings);
        w8(&w, h->minutes_observed);
        w64(&w, h->minute_bits);
    }
    for (int i = 0; i < FW_AN_DAYS; ++i) {
        const FwAnDay *d = &a->days[i];
        w32(&w, (uint32_t)d->day);
        w16(&w, d->sightings);
        w16(&w, d->minutes_observed);
        w16(&w, d->routes_known);
        for (int k = 0; k < FW_AN_TOP; ++k) {
            wbytes(&w, d->top[k].code, FW_AN_CODE);
            w16(&w, d->top[k].count);
            w16(&w, d->top[k].error);
        }
        wtags(&w, d->types, FW_AN_DIM_TOP);
        wtags(&w, d->orig, FW_AN_DIM_TOP);
        wtags(&w, d->dest, FW_AN_DIM_TOP);
    }
    /* Rare sightings ring */
    w8(&w, a->rare_head);
    w8(&w, a->rare_count);
    w16(&w, a->rare_total);
    for (int i = 0; i < FW_AN_RARE; ++i) {
        const FwAnRare *r = &a->rare[i];
        w32(&w, r->seen_at);
        w32(&w, r->key);
        w8(&w, r->tier);
        wbytes(&w, r->ac_type, sizeof r->ac_type);
        wbytes(&w, r->cs, sizeof r->cs);
        wbytes(&w, r->reg, sizeof r->reg);
        wbytes(&w, r->al, sizeof r->al);
        wbytes(&w, r->orig, sizeof r->orig);
        wbytes(&w, r->dest, sizeof r->dest);
    }
    /* Heatmap: sparse non-zero cells (3 bytes each) or dense when > 533 cells */
    wf32(&w, a->heat.lat);
    wf32(&w, a->heat.lon);
    wf32(&w, a->heat.dlat);
    wf32(&w, a->heat.dlon);
    w32(&w, a->heat.since);
    w16(&w, a->heat.scale ? a->heat.scale : 1);
    w16(&w, a->heat.points);
    for (int i = 0; i < FW_AN_GRID * FW_AN_GRID; ++i)
        if (a->heat.cells[i]) ++nonzero;
    if ((size_t)nonzero * 3u < sizeof a->heat.cells) {
        w8(&w, 1); /* sparse */
        w16(&w, nonzero);
        for (int i = 0; i < FW_AN_GRID * FW_AN_GRID; ++i) {
            if (!a->heat.cells[i]) continue;
            w16(&w, (uint16_t)i);
            w8(&w, a->heat.cells[i]);
        }
    } else {
        w8(&w, 0); /* dense */
        wbytes(&w, a->heat.cells, sizeof a->heat.cells);
    }
    if (!w.ok || w.len + 4 > cap) return 0;
    w32(&w, crc32(buf, w.len));
    return w.len;
}

static bool parse(const uint8_t *buf, size_t len, FwAnalytics *out){
    R r = {buf, len - 4, 0, true};
    uint32_t sightings, observations;
    uint8_t rhead, rcnt, mode;
    uint16_t rtot, scale, pts;
    float hlat, hlon, hdlat, hdlon;
    uint32_t hsince;
    if (r32(&r) != MAGIC || r16(&r) != VERSION || r8(&r) != FW_AN_HOURS || r8(&r) != FW_AN_DAYS ||
        r8(&r) != FW_AN_TOP || r8(&r) != FW_AN_DIM_TOP || r8(&r) != FW_AN_RARE || r8(&r) != FW_AN_GRID)
        return false;
    sightings = r32(&r);
    observations = r32(&r);
    if (out) {
        out->total_sightings = sightings;
        out->total_observations = observations;
    }
    for (int i = 0; i < FW_AN_HOURS; ++i) {
        FwAnHour h;
        memset(&h, 0, sizeof h);
        h.hour = (int32_t)r32(&r);
        h.sightings = (uint16_t)r16(&r);
        h.minutes_observed = (uint8_t)r8(&r);
        h.minute_bits = r64(&r);
        if (h.hour < -1 || (h.hour >= 0 && ring(h.hour, FW_AN_HOURS) != i) || h.minutes_observed > 60) return false;
        if (out) out->hours[i] = h;
    }
    for (int i = 0; i < FW_AN_DAYS; ++i) {
        FwAnDay d;
        memset(&d, 0, sizeof d);
        d.day = (int32_t)r32(&r);
        d.sightings = (uint16_t)r16(&r);
        d.minutes_observed = (uint16_t)r16(&r);
        d.routes_known = (uint16_t)r16(&r);
        for (int k = 0; k < FW_AN_TOP; ++k) {
            rbytes(&r, d.top[k].code, FW_AN_CODE);
            d.top[k].code[FW_AN_CODE - 1] = 0;
            d.top[k].count = (uint16_t)r16(&r);
            d.top[k].error = (uint16_t)r16(&r);
        }
        rtags(&r, d.types, FW_AN_DIM_TOP);
        rtags(&r, d.orig, FW_AN_DIM_TOP);
        rtags(&r, d.dest, FW_AN_DIM_TOP);
        if (d.day < -1 || (d.day >= 0 && ring(d.day, FW_AN_DAYS) != i) || d.minutes_observed > 1440) return false;
        if (out) out->days[i] = d;
    }
    rhead = (uint8_t)r8(&r);
    rcnt = (uint8_t)r8(&r);
    rtot = (uint16_t)r16(&r);
    if (rhead >= FW_AN_RARE || rcnt > FW_AN_RARE) return false;
    if (out) { out->rare_head = rhead; out->rare_count = rcnt; out->rare_total = rtot; }
    for (int i = 0; i < FW_AN_RARE; ++i) {
        FwAnRare rr;
        memset(&rr, 0, sizeof rr);
        rr.seen_at = r32(&r);
        rr.key = r32(&r);
        rr.tier = (uint8_t)r8(&r);
        if (rr.tier > FW_AN_RARE_ONE_OF_KIND) return false;
        rbytes(&r, rr.ac_type, sizeof rr.ac_type); rr.ac_type[sizeof rr.ac_type - 1] = 0;
        rbytes(&r, rr.cs, sizeof rr.cs); rr.cs[sizeof rr.cs - 1] = 0;
        rbytes(&r, rr.reg, sizeof rr.reg); rr.reg[sizeof rr.reg - 1] = 0;
        rbytes(&r, rr.al, sizeof rr.al); rr.al[sizeof rr.al - 1] = 0;
        rbytes(&r, rr.orig, sizeof rr.orig); rr.orig[sizeof rr.orig - 1] = 0;
        rbytes(&r, rr.dest, sizeof rr.dest); rr.dest[sizeof rr.dest - 1] = 0;
        if (out) out->rare[i] = rr;
    }
    hlat = rf32(&r);
    hlon = rf32(&r);
    hdlat = rf32(&r);
    hdlon = rf32(&r);
    hsince = r32(&r);
    scale = (uint16_t)r16(&r);
    pts = (uint16_t)r16(&r);
    mode = (uint8_t)r8(&r);
    if (hlat != hlat || hlon != hlon || hdlat != hdlat || hdlon != hdlon || hlat < -90.0f || hlat > 90.0f ||
        hlon < -180.0f || hlon > 180.0f || hdlat < 0.0f || hdlat > 10.0f || hdlon < 0.0f || hdlon > 16.0f || !scale)
        return false;
    if (out) {
        out->heat.lat = hlat;
        out->heat.lon = hlon;
        out->heat.dlat = hdlat;
        out->heat.dlon = hdlon;
        out->heat.since = hsince;
        out->heat.scale = scale;
        out->heat.points = pts;
        memset(out->heat.cells, 0, sizeof out->heat.cells);
    }
    if (mode == 1) {
        uint16_t cnt = (uint16_t)r16(&r);
        if (cnt > FW_AN_GRID * FW_AN_GRID) return false;
        for (uint16_t i = 0; i < cnt; ++i) {
            uint16_t idx = (uint16_t)r16(&r);
            uint8_t val = (uint8_t)r8(&r);
            if (idx >= FW_AN_GRID * FW_AN_GRID) return false;
            if (out) out->heat.cells[idx] = val;
        }
    } else if (mode == 0) {
        if (out) rbytes(&r, out->heat.cells, sizeof out->heat.cells);
        else {
            if (r.pos + sizeof out->heat.cells > r.len) return false;
            r.pos += sizeof out->heat.cells;
        }
    } else {
        return false;
    }
    return r.ok && r.pos == r.len;
}

bool fw_an_load(FwAnalytics *a, const uint8_t *buf, size_t len){
    if (!buf || len < 4 || crc32(buf, len - 4) != ((uint32_t)buf[len - 4] | (uint32_t)buf[len - 3] << 8 |
                                                    (uint32_t)buf[len - 2] << 16 | (uint32_t)buf[len - 1] << 24))
        return false;
    if (!parse(buf, len, NULL)) return false;
    parse(buf, len, a);                              /* the dedup/track tables stay live */
    return true;
}
