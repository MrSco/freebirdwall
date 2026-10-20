/* The owned Sync schedule; see fw_schedule.h. */
#include "fw_schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define BLOB_MAGIC 0x31534746u        /* "FGS1" */
#define LEG_BYTES 32

void fw_sched_init(FwSchedule *s){
    memset(s, 0, sizeof *s);
    s->show_upcoming = true;
}

/* ------------------------------------------------------------ airline codes */

/* Common carriers, IATA -> ICAO (public designators). */
static const char AIRLINES[][2][4] = {
    {"AA", "AAL"}, {"DL", "DAL"}, {"UA", "UAL"}, {"WN", "SWA"}, {"B6", "JBU"}, {"AS", "ASA"}, {"NK", "NKS"},
    {"F9", "FFT"}, {"G4", "AAY"}, {"HA", "HAL"}, {"SY", "SCX"}, {"MX", "MXY"}, {"XP", "CXP"}, {"9E", "EDV"},
    {"OH", "JIA"}, {"YX", "RPA"}, {"OO", "SKW"}, {"MQ", "ENY"}, {"YV", "ASH"}, {"QX", "QXE"}, {"PT", "PDT"},
    {"C5", "UCA"}, {"G7", "GJS"}, {"ZW", "AWI"}, {"AX", "LOF"}, {"EV", "ASQ"}, {"KS", "PEN"}, {"3M", "SIL"},
    {"AC", "ACA"}, {"WS", "WJA"}, {"PD", "POE"}, {"TS", "TSC"}, {"F8", "FLE"}, {"AM", "AMX"}, {"Y4", "VOI"},
    {"VB", "VIV"}, {"CM", "CMP"}, {"AV", "AVA"}, {"LA", "LAN"}, {"JJ", "TAM"}, {"G3", "GLO"}, {"AD", "AZU"},
    {"AR", "ARG"}, {"BW", "BWA"}, {"BA", "BAW"}, {"VS", "VIR"}, {"U2", "EZY"}, {"FR", "RYR"}, {"LH", "DLH"},
    {"AF", "AFR"}, {"KL", "KLM"}, {"IB", "IBE"}, {"AY", "FIN"}, {"SK", "SAS"}, {"LX", "SWR"}, {"OS", "AUA"},
    {"SN", "BEL"}, {"TP", "TAP"}, {"AZ", "ITY"}, {"EI", "EIN"}, {"TK", "THY"}, {"EK", "UAE"}, {"QR", "QTR"},
    {"EY", "ETD"}, {"SV", "SVA"}, {"MS", "MSR"}, {"ET", "ETH"}, {"KQ", "KQA"}, {"SA", "SAA"}, {"AI", "AIC"},
    {"6E", "IGO"}, {"SQ", "SIA"}, {"CX", "CPA"}, {"JL", "JAL"}, {"NH", "ANA"}, {"KE", "KAL"}, {"OZ", "AAR"},
    {"CA", "CCA"}, {"MU", "CES"}, {"CZ", "CSN"}, {"QF", "QFA"}, {"VA", "VOZ"}, {"NZ", "ANZ"}, {"PR", "PAL"},
    {"TG", "THA"}, {"MH", "MAS"}, {"GA", "GIA"}, {"BR", "EVA"}, {"CI", "CAL"}, {"5X", "UPS"}, {"FX", "FDX"},
    {"5Y", "GTI"}, {"K4", "CKS"}, {"QY", "BCS"}, {"GB", "ABX"}, {"PO", "PAC"}, {"NC", "NAC"},
};

const char *fw_sched_icao_airline(const char *code){
    size_t n = code ? strlen(code) : 0;
    if (n == 3) return code;
    if (n != 2) return NULL;
    for (size_t i = 0; i < sizeof AIRLINES / sizeof AIRLINES[0]; ++i)
        if (!strcmp(AIRLINES[i][0], code)) return AIRLINES[i][1];
    return NULL;
}

/* ------------------------------------------------------------ view */

void fw_sched_callsign(const char *number, char *cs, size_t cap){
    const char *icao = NULL, *n;
#define FW_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
    if (strlen(number) > 2 && !(FW_UPPER(number[0]) && FW_UPPER(number[1]) && FW_UPPER(number[2]))) {
        char al[3] = {number[0], number[1], 0};
        icao = fw_sched_icao_airline(al);
    }
#undef FW_UPPER
    if (!icao || strlen(icao) != 3) { snprintf(cs, cap, "%s", number); return; }
    n = number + 2;
    while (n[0] == '0' && n[1]) ++n;
    snprintf(cs, cap, "%s%s", icao, n);
}

bool fw_sched_live_window(const FwSyncLeg *leg, uint32_t now){
    uint32_t end;
    if (!leg->dep_at || fw_sync_canceled(leg)) return false;
    end = (leg->arr_at > leg->dep_at ? leg->arr_at : leg->dep_at + 6 * 3600) + FW_SCHED_LIVE_MARGIN;
    return now + FW_SCHED_LIVE_MARGIN >= leg->dep_at && now <= end;
}

void fw_sched_rel(uint32_t t, uint32_t now, char *out, size_t cap){
    bool past = now > t;
    uint32_t m = (past ? now - t : t - now) / 60;
    if (m >= 1440) snprintf(out, cap, past ? "%ud ago" : "in %ud", (unsigned)(m / 1440));
    else if (m >= 60) snprintf(out, cap, past ? "%uh %02um ago" : "in %uh %02um", (unsigned)(m / 60), (unsigned)(m % 60));
    else snprintf(out, cap, past ? "%um ago" : "in %um", (unsigned)m);
}

static bool visible(const FwSchedule *s, const FwSchedLeg *l){
    if (l->type == FW_LEG_DEADHEAD) return s->show_deadhead;
    if (l->type == FW_LEG_RESERVE) return s->show_reserve;
    return true;
}

static void to_sync(const FwSchedLeg *l, uint32_t now, FwSyncLeg *o){
    static const char *const types[] = {"operating", "deadhead", "reserve"};
    fw_sync_leg_init(o);
    snprintf(o->lt, sizeof o->lt, "%s", types[l->type < 3 ? l->type : 0]);
    if (l->canceled) snprintf(o->ls, sizeof o->ls, "canceled");
    snprintf(o->al, sizeof o->al, "%s", l->al);
    snprintf(o->fn, sizeof o->fn, "%s", l->fn);
    snprintf(o->orig, sizeof o->orig, "%s", l->orig);
    snprintf(o->dest, sizeof o->dest, "%s", l->dest);
    if (l->dep_at) fw_sched_rel(l->dep_at, now, o->dep_rel, sizeof o->dep_rel);
    if (l->arr_at) fw_sched_rel(l->arr_at, now, o->arr_rel, sizeof o->arr_rel);
    o->dep_at = l->dep_at;
    o->arr_at = l->arr_at;
}

void fw_sched_view(const FwSchedule *s, uint32_t now, FwSchedView *out){
    const FwSchedLeg *active = NULL;
    memset(out, 0, sizeof *out);
    out->message = s->message[0] ? s->message : NULL;
    if (!s->enabled) return;
    /* the active leg: in the air; else one that landed less than 30 min ago;
     * else the next one within 2 h of departure. Never a canceled one. */
    for (int i = 0; i < s->count && !active; ++i) {
        const FwSchedLeg *l = &s->legs[i];
        if (visible(s, l) && !l->canceled && l->dep_at <= now && now < l->arr_at) active = l;
    }
    for (int i = s->count - 1; i >= 0 && !active; --i) {
        const FwSchedLeg *l = &s->legs[i];
        if (visible(s, l) && !l->canceled && l->arr_at <= now && now - l->arr_at < FW_SCHED_ACTIVE_AFTER_S) active = l;
    }
    for (int i = 0; i < s->count && !active; ++i) {
        const FwSchedLeg *l = &s->legs[i];
        if (visible(s, l) && !l->canceled && now < l->dep_at && l->dep_at - now <= FW_SCHED_ACTIVE_BEFORE_S) active = l;
    }
    if (active) { out->has_active = true; to_sync(active, now, &out->active); }
    /* upcoming: departures still ahead, in order, canceled ones included
     * (they show "Canceled"), the active leg left out */
    for (int i = 0; i < s->count && out->upcoming_count < FW_SYNC_UPCOMING_MAX; ++i) {
        const FwSchedLeg *l = &s->legs[i];
        if (l == active || !visible(s, l) || l->dep_at <= now) continue;
        to_sync(l, now, &out->upcoming[out->upcoming_count++]);
    }
}

/* ------------------------------------------------------------ JSON */

typedef struct { char *buf; size_t cap, len; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void outf(Out *o, const char *fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(o->len < o->cap ? o->buf + o->len : NULL, o->len < o->cap ? o->cap - o->len : 0, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n;
}

static void out_str(Out *o, const char *s){
    outf(o, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') outf(o, "\\%c", c);
        else if (c < 0x20) outf(o, "\\u%04x", c);
        else outf(o, "%c", c);
    }
    outf(o, "\"");
}

/* days from civil (Howard Hinnant), for ISO output without the C library's time zone */
static void iso(uint32_t t, char out[40]){
    int64_t z = t / 86400 + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    unsigned sec = t % 86400;
    snprintf(out, 40, "%04d-%02u-%02uT%02u:%02uZ", (int)y, m, d, sec / 3600, sec / 60 % 60);
}

size_t fw_sched_json(const FwSchedule *s, char *buf, size_t cap){
    static const char *const types[] = {"operating", "deadhead", "reserve"};
    Out o = {buf, cap, 0};
    char a[40], b[40];
    if (cap) buf[0] = 0;
    outf(&o, "{\"enabled\":%s,\"show_upcoming\":%s,\"main_view\":\"%s\",\"upcoming_view\":\"%s\",\"show_deadhead\":%s,"
             "\"show_reserve\":%s,\"message\":", s->enabled ? "true" : "false", s->show_upcoming ? "true" : "false",
         s->message_view ? "message" : "active", s->list_view ? "list" : "next", s->show_deadhead ? "true" : "false",
         s->show_reserve ? "true" : "false");
    out_str(&o, s->message);
    outf(&o, ",\"legs\":[");
    for (int i = 0; i < s->count; ++i) {
        const FwSchedLeg *l = &s->legs[i];
        iso(l->dep_at, a);
        iso(l->arr_at, b);
        outf(&o, "%s{\"al\":\"%s\",\"fn\":\"%s\",\"orig\":\"%s\",\"dest\":\"%s\",\"dep\":\"%s\",\"arr\":\"%s\",\"type\":\"%s\","
                 "\"canceled\":%s}", i ? "," : "", l->al, l->fn, l->orig, l->dest, a, b, types[l->type < 3 ? l->type : 0],
             l->canceled ? "true" : "false");
    }
    outf(&o, "]}");
    if (cap && o.len >= cap) buf[cap - 1] = 0;
    return o.len;
}

static bool fail(char *why, size_t cap, const char *msg){
    if (why && cap) snprintf(why, cap, "%s", msg);
    return false;
}

static bool code_ok(const char *s, size_t lo, size_t hi){
    size_t n = s ? strlen(s) : 0;
    if (n < lo || n > hi) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}

static const char *str(const cJSON *o, const char *k){
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool flag(const cJSON *o, const char *k, bool dflt, bool *ok){
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v) return dflt;
    if (!cJSON_IsBool(v)) { *ok = false; return dflt; }
    return cJSON_IsTrue(v);
}

static int by_departure(const void *a, const void *b){
    const FwSchedLeg *x = a, *y = b;
    int c;
    if (x->dep_at != y->dep_at) return x->dep_at < y->dep_at ? -1 : 1;
    if ((c = strcmp(x->al, y->al)) != 0) return c;          /* same time: a fixed order, so saves round-trip */
    return strcmp(x->fn, y->fn);
}

#define s (*sp)
bool fw_sched_parse(const char *json, FwSchedule *out, char *why, size_t why_cap){
    FwSchedule *sp = malloc(sizeof *sp);  /* ~1.6 KB for the moment of parsing: not on the stack, not kept */
    cJSON *root;
    if (!sp) return fail(why, why_cap, "out of memory");
    root = json ? cJSON_Parse(json) : NULL;
    const cJSON *legs, *l;
    const char *v;
    bool ok = true;
    fw_sched_init(&s);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "not a JSON object"); }
    s.enabled = flag(root, "enabled", false, &ok);
    s.show_upcoming = flag(root, "show_upcoming", true, &ok);
    s.show_deadhead = flag(root, "show_deadhead", false, &ok);
    s.show_reserve = flag(root, "show_reserve", false, &ok);
    if (!ok) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "switches must be true or false"); }
    v = str(root, "main_view");
    if (v && strcmp(v, "active") && strcmp(v, "message")) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "main_view: active or message"); }
    s.message_view = v && !strcmp(v, "message");
    v = str(root, "upcoming_view");
    if (v && strcmp(v, "next") && strcmp(v, "list")) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "upcoming_view: next or list"); }
    s.list_view = v && !strcmp(v, "list");
    v = str(root, "message");
    if (v) {
        if (strlen(v) >= sizeof s.message) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "message: at most 255 bytes"); }
        for (const char *p = v; *p; ++p)
            if ((unsigned char)*p < 0x20) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "message: one line of text"); }
        snprintf(s.message, sizeof s.message, "%s", v);
    }
    legs = cJSON_GetObjectItemCaseSensitive(root, "legs");
    if (legs && !cJSON_IsArray(legs)) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "legs must be a list"); }
    if (cJSON_GetArraySize(legs) > FW_SCHED_MAX) { cJSON_Delete(root); free(sp); return fail(why, why_cap, "at most 20 legs"); }
    cJSON_ArrayForEach(l, legs) {
        FwSchedLeg *g = &s.legs[s.count];
        const char *t, *al = str(l, "al"), *fn = str(l, "fn"), *orig = str(l, "orig"), *dest = str(l, "dest");
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(l, "canceled");
        char msg[64];
        int n = s.count + 1;
        if (!cJSON_IsObject(l)) { snprintf(msg, sizeof msg, "leg %d: not an object", n); goto bad; }
        if (!code_ok(al, 2, 3)) { snprintf(msg, sizeof msg, "leg %d: airline is 2-3 letters/digits", n); goto bad; }
        if (!code_ok(fn, 1, 5) || fn[0] < '0' || fn[0] > '9') { snprintf(msg, sizeof msg, "leg %d: flight number like 1234", n); goto bad; }
        if (!code_ok(orig ? orig : "", 0, 4) || (orig && *orig && strlen(orig) < 3) ||
            !code_ok(dest ? dest : "", 0, 4) || (dest && *dest && strlen(dest) < 3)) {
            snprintf(msg, sizeof msg, "leg %d: airports are 3-4 letter codes", n); goto bad;
        }
        g->dep_at = fw_sync_parse_time(str(l, "dep") ? str(l, "dep") : "");
        g->arr_at = fw_sync_parse_time(str(l, "arr") ? str(l, "arr") : "");
        if (!g->dep_at || !g->arr_at) { snprintf(msg, sizeof msg, "leg %d: departure and arrival times", n); goto bad; }
        if (g->arr_at <= g->dep_at || g->arr_at - g->dep_at > 24 * 3600u) {
            snprintf(msg, sizeof msg, "leg %d: arrival must be after departure, within 24 h", n); goto bad;
        }
        t = str(l, "type");
        if (!t || !strcmp(t, "operating")) g->type = FW_LEG_OPERATING;
        else if (!strcmp(t, "deadhead")) g->type = FW_LEG_DEADHEAD;
        else if (!strcmp(t, "reserve")) g->type = FW_LEG_RESERVE;
        else { snprintf(msg, sizeof msg, "leg %d: type operating, deadhead or reserve", n); goto bad; }
        if (c && !cJSON_IsBool(c)) { snprintf(msg, sizeof msg, "leg %d: canceled is true or false", n); goto bad; }
        g->canceled = cJSON_IsTrue(c) ? 1 : 0;
        snprintf(g->al, sizeof g->al, "%s", al);
        snprintf(g->fn, sizeof g->fn, "%s", fn);
        snprintf(g->orig, sizeof g->orig, "%s", orig ? orig : "");
        snprintf(g->dest, sizeof g->dest, "%s", dest ? dest : "");
        ++s.count;
        continue;
bad:
        cJSON_Delete(root);
        free(sp);
        return fail(why, why_cap, msg);
    }
    cJSON_Delete(root);
    qsort(s.legs, (size_t)s.count, sizeof s.legs[0], by_departure);
    *out = s;
    free(sp);
    if (why && why_cap) why[0] = 0;
    return true;
}
#undef s

/* ------------------------------------------------------------ storage */

static void put32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p){ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t crc32(const uint8_t *p, size_t n){
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}

/* magic, flags, count, legs (32 bytes each), message length + text, CRC */
size_t fw_sched_save(const FwSchedule *s, uint8_t *buf, size_t cap){
    size_t msg = strlen(s->message), n = 12 + (size_t)s->count * LEG_BYTES + 2 + msg + 4, at = 12;
    if (n > cap || s->count < 0 || s->count > FW_SCHED_MAX) return 0;
    memset(buf, 0, n);
    put32(buf, BLOB_MAGIC);
    buf[4] = (uint8_t)(s->enabled | s->show_upcoming << 1 | s->message_view << 2 | s->list_view << 3 | s->show_deadhead << 4 |
                       s->show_reserve << 5);
    buf[8] = (uint8_t)s->count;
    for (int i = 0; i < s->count; ++i, at += LEG_BYTES) {
        const FwSchedLeg *l = &s->legs[i];
        memcpy(buf + at, l->al, 4);
        memcpy(buf + at + 4, l->fn, 6);
        memcpy(buf + at + 10, l->orig, 5);
        memcpy(buf + at + 15, l->dest, 5);
        buf[at + 20] = l->type;
        buf[at + 21] = l->canceled;
        put32(buf + at + 24, l->dep_at);
        put32(buf + at + 28, l->arr_at);
    }
    buf[at] = (uint8_t)msg; buf[at + 1] = (uint8_t)(msg >> 8);
    memcpy(buf + at + 2, s->message, msg);
    at += 2 + msg;
    put32(buf + at, crc32(buf, at));
    return at + 4;
}

#define t (*tp)
bool fw_sched_load(FwSchedule *s, const uint8_t *buf, size_t len){
    FwSchedule *tp;
    size_t at = 12, msg;
    int count;
    if (len < 18 || get32(buf) != BLOB_MAGIC || get32(buf + len - 4) != crc32(buf, len - 4)) return false;
    count = buf[8];
    if (count > FW_SCHED_MAX || len < 12 + (size_t)count * LEG_BYTES + 2 + 4) return false;
    if (!(tp = malloc(sizeof *tp))) return false;
    fw_sched_init(&t);
    t.enabled = buf[4] & 1; t.show_upcoming = buf[4] >> 1 & 1; t.message_view = buf[4] >> 2 & 1;
    t.list_view = buf[4] >> 3 & 1; t.show_deadhead = buf[4] >> 4 & 1; t.show_reserve = buf[4] >> 5 & 1;
    for (int i = 0; i < count; ++i, at += LEG_BYTES) {
        FwSchedLeg *l = &t.legs[i];
        memcpy(l->al, buf + at, 4); l->al[3] = 0;
        memcpy(l->fn, buf + at + 4, 6); l->fn[5] = 0;
        memcpy(l->orig, buf + at + 10, 5); l->orig[4] = 0;
        memcpy(l->dest, buf + at + 15, 5); l->dest[4] = 0;
        l->type = buf[at + 20] < 3 ? buf[at + 20] : 0;
        l->canceled = buf[at + 21] ? 1 : 0;
        l->dep_at = get32(buf + at + 24);
        l->arr_at = get32(buf + at + 28);
    }
    t.count = count;
    msg = buf[at] | (size_t)buf[at + 1] << 8;
    if (msg >= sizeof t.message || at + 2 + msg + 4 != len) { free(tp); return false; }
    memcpy(t.message, buf + at + 2, msg);
    t.message[msg] = 0;
    *s = t;
    free(tp);
    return true;
}
#undef t
