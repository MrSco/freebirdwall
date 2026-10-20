/* The vendor feed response in MessagePack, read with the stock firmware's type
 * rules (FUN_4200d594, records FUN_4200cee0, legs FUN_4200cd8c; FEED.md):
 *  - integers: positive fixint, u8..u64, i8..i64 and nil; negative fixint and
 *    floats are errors;
 *  - doubles: f32, f64, any integer; nil -> NAN;
 *  - strings: fixstr/str8/16/32; nil -> ""; cut bytewise to the field size;
 *  - bools: true/false, positive fixint; nil -> false;
 *  - unknown keys are skipped (nesting up to 16);
 *  - any wrongly typed value rejects the whole response.
 * Keys are compared after the same truncation stock applies: 3 characters at
 * the top level, 11 in a record, 7 in trk_info / deltas. */
#include "fw_feed.h"
#include <math.h>
#include <string.h>

typedef struct {
    const uint8_t *p, *end;
    bool bad;
} Rd;

static bool need(Rd *r, size_t n){
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = true; return false; }
    return true;
}

static uint64_t be(Rd *r, int n){
    uint64_t v = 0;
    if (!need(r, (size_t)n)) return 0;
    for (int i = 0; i < n; ++i) v = v << 8 | r->p[i];
    r->p += n;
    return v;
}

static uint8_t byte(Rd *r){ return need(r, 1) ? *r->p++ : 0; }

static bool peek_nil(Rd *r){
    if (need(r, 1) && *r->p == 0xc0) { ++r->p; return true; }
    return false;
}

static int64_t read_int(Rd *r){
    uint8_t t;
    if (!need(r, 1)) return 0;
    t = byte(r);
    if (t <= 0x7f) return t;
    switch (t) {
    case 0xc0: return 0;
    case 0xcc: return (int64_t)be(r, 1);
    case 0xcd: return (int64_t)be(r, 2);
    case 0xce: return (int64_t)be(r, 4);
    case 0xcf: return (int64_t)be(r, 8);
    case 0xd0: return (int8_t)be(r, 1);
    case 0xd1: return (int16_t)be(r, 2);
    case 0xd2: return (int32_t)be(r, 4);
    case 0xd3: return (int64_t)be(r, 8);
    default: r->bad = true; return 0;
    }
}

static double read_double(Rd *r){
    uint8_t t;
    if (!need(r, 1)) return NAN;
    t = *r->p;
    if (t == 0xc0) { ++r->p; return NAN; }
    if (t == 0xca) {
        uint32_t u;
        float f;
        ++r->p;
        u = (uint32_t)be(r, 4);
        memcpy(&f, &u, sizeof f);
        return f;
    }
    if (t == 0xcb) {
        uint64_t u;
        double d;
        ++r->p;
        u = be(r, 8);
        memcpy(&d, &u, sizeof d);
        return d;
    }
    if (t >= 0xe0) { ++r->p; return (int8_t)t; }   /* negative fixint is a number here */
    if (t == 0xcf) { ++r->p; return (double)be(r, 8); }
    return (double)read_int(r);
}

static bool read_bool(Rd *r){
    uint8_t t;
    if (!need(r, 1)) return false;
    t = byte(r);
    if (t == 0xc3) return true;
    if (t == 0xc2 || t == 0xc0) return false;
    if (t <= 0x7f) return t != 0;
    r->bad = true;
    return false;
}

/* String into out[cap] cut bytewise to cap-1; returns false (bad) on a type error. */
static void read_str(Rd *r, char *out, size_t cap){
    uint8_t t;
    size_t n;
    if (cap) out[0] = 0;
    if (!need(r, 1)) return;
    t = byte(r);
    if ((t & 0xe0) == 0xa0) n = t & 0x1f;
    else if (t == 0xd9) n = (size_t)be(r, 1);
    else if (t == 0xda) n = (size_t)be(r, 2);
    else if (t == 0xdb) n = (size_t)be(r, 4);
    else if (t == 0xc0) return;
    else { r->bad = true; return; }
    if (!need(r, n)) return;
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(out, r->p, k);
        out[k] = 0;
    }
    r->p += n;
}

static void skip(Rd *r, int depth);

static void skip_n(Rd *r, uint64_t n, int depth){
    for (uint64_t i = 0; i < n && !r->bad; ++i) skip(r, depth);
}

static void skip(Rd *r, int depth){
    uint8_t t;
    if (depth > 16) { r->bad = true; return; }
    if (!need(r, 1)) return;
    t = byte(r);
    if (t <= 0x7f || t >= 0xe0 || t == 0xc0 || t == 0xc2 || t == 0xc3) return;
    if ((t & 0xe0) == 0xa0) { if (need(r, t & 0x1f)) r->p += t & 0x1f; return; }
    if ((t & 0xf0) == 0x90) { skip_n(r, t & 0x0f, depth + 1); return; }
    if ((t & 0xf0) == 0x80) { skip_n(r, 2u * (t & 0x0f), depth + 1); return; }
    switch (t) {
    case 0xcc: case 0xd0: r->p += need(r, 1) ? 1 : 0; return;
    case 0xcd: case 0xd1: r->p += need(r, 2) ? 2 : 0; return;
    case 0xce: case 0xd2: case 0xca: r->p += need(r, 4) ? 4 : 0; return;
    case 0xcf: case 0xd3: case 0xcb: r->p += need(r, 8) ? 8 : 0; return;
    case 0xd9: case 0xc4: { size_t n = (size_t)be(r, 1); if (need(r, n)) r->p += n; return; }
    case 0xda: case 0xc5: { size_t n = (size_t)be(r, 2); if (need(r, n)) r->p += n; return; }
    case 0xdb: case 0xc6: { size_t n = (size_t)be(r, 4); if (need(r, n)) r->p += n; return; }
    case 0xdc: skip_n(r, be(r, 2), depth + 1); return;
    case 0xdd: skip_n(r, be(r, 4), depth + 1); return;
    case 0xde: skip_n(r, 2 * be(r, 2), depth + 1); return;
    case 0xdf: skip_n(r, 2 * be(r, 4), depth + 1); return;
    default: r->bad = true; return;                 /* ext types: unsupported */
    }
}

/* Map/array headers; nil reads as an empty container. */
static uint32_t read_map(Rd *r){
    uint8_t t;
    if (!need(r, 1)) return 0;
    t = byte(r);
    if ((t & 0xf0) == 0x80) return t & 0x0f;
    if (t == 0xde) return (uint32_t)be(r, 2);
    if (t == 0xdf) return (uint32_t)be(r, 4);
    if (t == 0xc0) return 0;
    r->bad = true;
    return 0;
}

static uint32_t read_array(Rd *r){
    uint8_t t;
    if (!need(r, 1)) return 0;
    t = byte(r);
    if ((t & 0xf0) == 0x90) return t & 0x0f;
    if (t == 0xdc) return (uint32_t)be(r, 2);
    if (t == 0xdd) return (uint32_t)be(r, 4);
    if (t == 0xc0) return 0;
    r->bad = true;
    return 0;
}

/* ------------------------------------------------------------ records */

static void clean_reg(char *reg){
    /* trim, then only [A-Za-z0-9-] and at most 15 characters, upper-cased */
    char *s = reg, *e;
    size_t n;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    n = (size_t)(e - s);
    if (n >= 16) { reg[0] = 0; return; }
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { reg[0] = 0; return; }
    }
    memmove(reg, s, n);
    reg[n] = 0;
    for (size_t i = 0; i < n; ++i) if (reg[i] >= 'a' && reg[i] <= 'z') reg[i] = (char)(reg[i] - 32);
}

static void read_trk(Rd *r, FwFlight *f){
    uint32_t n = read_map(r);
    for (uint32_t i = 0; i < n && !r->bad; ++i) {
        char key[8], sts[4];
        read_str(r, key, sizeof key);
        if (r->bad) return;
        f->has_trk = true;
        if (!strcmp(key, "sts")) {
            read_str(r, sts, sizeof sts);
            f->sts = (sts[0] == 'C' || sts[0] == 'D') && !sts[1] ? sts[0] : 0;
        } else if (!strcmp(key, "sexp")) {
            int64_t v = read_int(r);
            f->sexp = v >= 1 && v <= 0xffffffffLL ? v : 0;
        } else if (!strcmp(key, "prog")) f->prog = read_double(r);
        else if (!strcmp(key, "dist")) f->dist = read_double(r);
        else if (!strcmp(key, "conf")) read_str(r, f->conf, 16);
        else if (!strcmp(key, "dep")) read_str(r, f->dep, 24);
        else if (!strcmp(key, "arr")) read_str(r, f->arr, 24);
        else if (!strcmp(key, "over")) read_str(r, f->over, 32);
        else if (!strcmp(key, "tft")) read_str(r, f->tft, 24);
        else if (!strcmp(key, "gnd")) f->trk_gnd = read_bool(r);
        else skip(r, 2);
    }
}

static void read_deltas(Rd *r){
    uint32_t n = read_map(r);
    for (uint32_t i = 0; i < n && !r->bad; ++i) {
        char key[8];
        read_str(r, key, sizeof key);
        if (!strcmp(key, "alt_b") || !strcmp(key, "alt_g") || !strcmp(key, "vel") || !strcmp(key, "trk") ||
            !strcmp(key, "vr") || !strcmp(key, "dt"))
            (void)read_double(r);                     /* type-checked; not used by the screens */
        else skip(r, 2);
    }
}

static void read_record(Rd *r, FwFlight *f){
    uint32_t n = read_map(r);
    for (uint32_t i = 0; i < n && !r->bad; ++i) {
        char key[12];
        read_str(r, key, sizeof key);
        if (r->bad) return;
        if (!strcmp(key, "icao")) read_str(r, f->icao, 8);
        else if (!strcmp(key, "cs")) read_str(r, f->cs, 12);
        else if (!strcmp(key, "reg")) { char reg[32]; read_str(r, reg, sizeof reg); clean_reg(reg); memcpy(f->reg, reg, 16); }
        else if (!strcmp(key, "alt_b")) f->alt_b = read_double(r);
        else if (!strcmp(key, "alt_g")) f->alt_g = read_double(r);
        else if (!strcmp(key, "vel")) f->vel = read_double(r);
        else if (!strcmp(key, "trk")) f->trk = read_double(r);
        else if (!strcmp(key, "vr")) f->vr = read_double(r);
        else if (!strcmp(key, "gnd")) f->gnd = read_bool(r);
        else if (!strcmp(key, "sqk")) read_str(r, f->sqk, 8);
        else if (!strcmp(key, "orig_icao")) read_str(r, f->orig_icao, 8);
        else if (!strcmp(key, "orig_iata")) read_str(r, f->orig_iata, 8);
        else if (!strcmp(key, "dest_icao")) read_str(r, f->dest_icao, 8);
        else if (!strcmp(key, "dest_iata")) read_str(r, f->dest_iata, 8);
        else if (!strcmp(key, "orig")) read_str(r, f->orig, 32);
        else if (!strcmp(key, "dest")) read_str(r, f->dest, 32);
        else if (!strcmp(key, "al_code")) read_str(r, f->al_code, 8);
        else if (!strcmp(key, "al_paint")) read_str(r, f->al_paint, 8);
        else if (!strcmp(key, "al_op")) read_str(r, f->al_op, 8);
        else if (!strcmp(key, "al_name")) read_str(r, f->al_name, 24);
        else if (!strcmp(key, "ac_type")) read_str(r, f->ac_type, 16);
        else if (!strcmp(key, "ac_name")) read_str(r, f->ac_name, 32);
        else if (!strcmp(key, "ac_class")) read_str(r, f->ac_class, 16);
        else if (!strcmp(key, "trk_info")) read_trk(r, f);
        else if (!strcmp(key, "deltas")) read_deltas(r);
        else skip(r, 1);
    }
}

static void read_leg(Rd *r, FwSyncLeg *l){
    uint32_t n = read_map(r);
    fw_sync_leg_init(l);
    for (uint32_t i = 0; i < n && !r->bad; ++i) {
        char key[12], at[40];
        read_str(r, key, sizeof key);
        if (r->bad) return;
        if (!strcmp(key, "lt")) read_str(r, l->lt, sizeof l->lt);
        else if (!strcmp(key, "ls")) read_str(r, l->ls, sizeof l->ls);
        else if (!strcmp(key, "al")) read_str(r, l->al, sizeof l->al);
        else if (!strcmp(key, "fn")) read_str(r, l->fn, sizeof l->fn);
        else if (!strcmp(key, "orig")) read_str(r, l->orig, sizeof l->orig);
        else if (!strcmp(key, "dest")) read_str(r, l->dest, sizeof l->dest);
        else if (!strcmp(key, "dep_rel")) read_str(r, l->dep_rel, sizeof l->dep_rel);
        else if (!strcmp(key, "arr_rel")) read_str(r, l->arr_rel, sizeof l->arr_rel);
        else if (!strcmp(key, "dep_at")) { read_str(r, at, sizeof at); l->dep_at = fw_sync_parse_time(at); }
        else if (!strcmp(key, "arr_at")) { read_str(r, at, sizeof at); l->arr_at = fw_sync_parse_time(at); }
        else skip(r, 1);
    }
}

/* Cuts a trailing partial UTF-8 sequence (FUN_4200b1b0, scm only). */
static void utf8_tail(char *s){
    size_t n = strlen(s), i = n, need;
    unsigned char c;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80 && n - i < 3) --i;
    if (!i) return;
    c = (unsigned char)s[i - 1];
    if (c < 0x80) return;
    need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0;
}

int fw_feed_parse(const uint8_t *body, size_t len, FwFeedFrame *out){
    Rd r = {body, body + len, false};
    uint32_t n;
    int version_bad = 0;
    memset(out, 0, sizeof *out);
    if (!body || !len) return FW_FEED_ERR_FORMAT;
    if (!need(&r, 1) || !((*r.p & 0xf0) == 0x80 || *r.p == 0xde || *r.p == 0xdf)) return FW_FEED_ERR_FORMAT;
    n = read_map(&r);
    for (uint32_t i = 0; i < n && !r.bad; ++i) {
        char key[4];
        read_str(&r, key, sizeof key);
        if (r.bad) break;
        if (!strcmp(key, "v")) {
            if (read_int(&r) > 1) version_bad = 1;
        } else if (!strcmp(key, "f") || !strcmp(key, "tf")) {
            uint32_t m = read_array(&r);
            out->tracked = key[0] == 't';
            for (uint32_t k = 0; k < m && !r.bad; ++k) {
                if (out->count < FW_FEED_MAX_FLIGHTS) {
                    FwFlight *f = &out->flights[out->count++];
                    fw_flight_init(f);
                    read_record(&r, f);
                } else {
                    skip(&r, 1);
                }
            }
        } else if (!strcmp(key, "ch")) {
            read_str(&r, out->config_hash, sizeof out->config_hash);
        } else if (!strcmp(key, "ov")) {
            out->overflow = (int)(uint16_t)read_int(&r);
        } else if (!strcmp(key, "sla")) {
            if (peek_nil(&r)) continue;
            read_leg(&r, &out->active);
            out->has_active = true;
        } else if (!strcmp(key, "slu")) {
            uint32_t m = read_array(&r);
            for (uint32_t k = 0; k < m && !r.bad; ++k) {
                if (out->upcoming_count < FW_SYNC_UPCOMING_MAX) read_leg(&r, &out->upcoming[out->upcoming_count++]);
                else skip(&r, 1);
            }
        } else if (!strcmp(key, "scm")) {
            read_str(&r, out->message, sizeof out->message);
            utf8_tail(out->message);
        } else {
            skip(&r, 1);
        }
    }
    if (r.bad) {
        memset(out, 0, sizeof *out);
        return FW_FEED_ERR_FORMAT;
    }
    if (version_bad) {
        memset(out, 0, sizeof *out);
        return FW_FEED_ERR_VERSION;
    }
    return FW_FEED_OK;
}
