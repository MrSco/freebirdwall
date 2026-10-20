/* Bounded on-device flight analytics; see fw_analytics.h and ANALYTICS.md. */
#include "fw_analytics.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WAYS 8
#define SETS (FW_AN_RECENT / WAYS)
#define MAGIC 0x4e415746u            /* "FWAN" little endian */
#define VERSION 1

void fw_an_init(FwAnalytics *a){
    memset(a, 0, sizeof *a);
    for (int i = 0; i < FW_AN_HOURS; ++i) a->hours[i].hour = -1;
    for (int i = 0; i < FW_AN_DAYS; ++i) a->days[i].day = -1;
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

/* The slot for an hour/day, claimed when it holds an older one; NULL when it
 * holds a newer one (the observation is too old to keep). */
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

/* Space-saving top-K: an unseen airline replaces the smallest entry and
 * inherits its count as error, so counts are upper bounds within `error`. */
static void count_airline(FwAnDay *d, const char *airline){
    char code[FW_AN_CODE];
    int i, empty = -1, min = -1;
    size_t n = 0;
    for (; airline[n] && n < FW_AN_CODE - 1; ++n)
        code[n] = (char)(airline[n] >= 'a' && airline[n] <= 'z' ? airline[n] - 32 : airline[n]);
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

/* Deduplication: set-associative, least recently seen way replaced. */
static bool new_sighting(FwAnalytics *a, uint32_t key, uint32_t now){
    FwAnRecent *set = &a->recent[(key % SETS) * WAYS];
    FwAnRecent *victim = &set[0];
    for (int i = 0; i < WAYS; ++i) {
        if (set[i].key == key) {
            bool fresh = now - set[i].last_seen >= FW_AN_REVISIT_S;
            if (now > set[i].last_seen) set[i].last_seen = now;
            return fresh;
        }
        if (!set[i].key) { victim = &set[i]; break; }
        if (set[i].last_seen < victim->last_seen) victim = &set[i];
    }
    victim->key = key;
    victim->last_seen = now;
    return true;
}

bool fw_an_observe(FwAnalytics *a, uint32_t key, const char *airline, uint32_t now, int offset_min){
    FwAnHour *h;
    FwAnDay *d;
    ++a->total_observations;
    if (!key || !new_sighting(a, key, now)) return false;
    ++a->total_sightings;
    if ((h = hour_slot(a, fw_an_local_hour(now, offset_min))) && h->sightings < UINT16_MAX) ++h->sightings;
    if ((d = day_slot(a, fw_an_local_day(now, offset_min)))) {
        if (d->sightings < UINT16_MAX) ++d->sightings;
        if (airline) count_airline(d, airline);
    }
    return true;
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
    for (int i = 1; i < n; ++i)                       /* insertion sort: count desc, code asc */
        for (int j = i; j > 0; --j) {
            FwAnAirline *x = &out[j - 1], *y = &out[j], t;
            if (x->count > y->count || (x->count == y->count && strcmp(x->code, y->code) <= 0)) break;
            t = *x; *x = *y; *y = t;
        }
    return n;
}

/* ------------------------------------------------------------ export */

typedef struct { char *buf; size_t cap, len; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out *o, const char *fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(o->len < o->cap ? o->buf + o->len : NULL, o->len < o->cap ? o->cap - o->len : 0, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n;
}

size_t fw_an_json(const FwAnalytics *a, char *buf, size_t cap){
    Out o = {buf, cap, 0};
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    bool first = true;
    if (cap) buf[0] = 0;
    outf(&o, "{\"hours\":[");
    for (int i = 0; i < FW_AN_HOURS; ++i)
        if (a->hours[i].hour >= 0) {
            if (a->hours[i].hour < lo) lo = a->hours[i].hour;
            if (a->hours[i].hour > hi) hi = a->hours[i].hour;
        }
    for (int32_t h = lo; lo <= hi && h <= hi; ++h) {
        const FwAnHour *x = fw_an_hour(a, h);
        if (!x) continue;
        outf(&o, "%s[%ld,%u,%u]", first ? "" : ",", (long)h, (unsigned)x->sightings, (unsigned)x->minutes_observed);
        first = false;
    }
    outf(&o, "],\"days\":[");
    lo = INT32_MAX; hi = INT32_MIN; first = true;
    for (int i = 0; i < FW_AN_DAYS; ++i)
        if (a->days[i].day >= 0) {
            if (a->days[i].day < lo) lo = a->days[i].day;
            if (a->days[i].day > hi) hi = a->days[i].day;
        }
    for (int32_t d = lo; lo <= hi && d <= hi; ++d) {
        const FwAnDay *x = fw_an_day(a, d);
        FwAnAirline top[FW_AN_TOP];
        int n;
        if (!x) continue;
        n = fw_an_top_airlines(a, d, top);
        outf(&o, "%s[%ld,%u,%u,[", first ? "" : ",", (long)d, (unsigned)x->sightings, (unsigned)x->minutes_observed);
        for (int i = 0; i < n; ++i) outf(&o, "%s[\"%s\",%u]", i ? "," : "", top[i].code, (unsigned)top[i].count);
        outf(&o, "]]");
        first = false;
    }
    outf(&o, "]}");
    if (cap && o.len >= cap) buf[cap - 1] = 0;
    return o.len;
}

/* ------------------------------------------------------------ checkpoint */

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

typedef struct { const uint8_t *p; size_t len, pos; bool ok; } R;
static uint32_t r8(R *r){ if (r->pos >= r->len) { r->ok = false; return 0; } return r->p[r->pos++]; }
static uint32_t r16(R *r){ uint32_t v = r8(r); return v | r8(r) << 8; }
static uint32_t r32(R *r){ uint32_t v = r16(r); return v | r16(r) << 16; }
static uint64_t r64(R *r){ uint64_t v = r32(r); return v | (uint64_t)r32(r) << 32; }

size_t fw_an_save(const FwAnalytics *a, uint8_t *buf, size_t cap){
    W w = {buf, cap, 0, true};
    w32(&w, MAGIC);
    w16(&w, VERSION);
    w8(&w, FW_AN_HOURS);
    w8(&w, FW_AN_DAYS);
    w8(&w, FW_AN_TOP);
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
        for (int k = 0; k < FW_AN_TOP; ++k) {
            for (int c = 0; c < FW_AN_CODE; ++c) w8(&w, (uint8_t)d->top[k].code[c]);
            w16(&w, d->top[k].count);
            w16(&w, d->top[k].error);
        }
    }
    if (!w.ok || w.len + 4 > cap) return 0;
    w32(&w, crc32(buf, w.len));
    return w.len;
}

/* Reads a checkpoint; with out == NULL it only validates. */
static bool parse(const uint8_t *buf, size_t len, FwAnalytics *out){
    R r = {buf, len - 4, 0, true};
    uint32_t sightings, observations;
    if (r32(&r) != MAGIC || r16(&r) != VERSION || r8(&r) != FW_AN_HOURS || r8(&r) != FW_AN_DAYS ||
        r8(&r) != FW_AN_TOP)
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
        for (int k = 0; k < FW_AN_TOP; ++k) {
            for (int c = 0; c < FW_AN_CODE; ++c) d.top[k].code[c] = (char)r8(&r);
            d.top[k].code[FW_AN_CODE - 1] = 0;
            d.top[k].count = (uint16_t)r16(&r);
            d.top[k].error = (uint16_t)r16(&r);
        }
        if (d.day < -1 || (d.day >= 0 && ring(d.day, FW_AN_DAYS) != i) || d.minutes_observed > 1440) return false;
        if (out) out->days[i] = d;
    }
    return r.ok && r.pos == r.len;
}

bool fw_an_load(FwAnalytics *a, const uint8_t *buf, size_t len){
    if (!buf || len < 4 || crc32(buf, len - 4) != ((uint32_t)buf[len - 4] | (uint32_t)buf[len - 3] << 8 |
                                                    (uint32_t)buf[len - 2] << 16 | (uint32_t)buf[len - 1] << 24))
        return false;
    if (!parse(buf, len, NULL)) return false;
    parse(buf, len, a);                              /* the dedup table is not saved: the live one stays */
    return true;
}
