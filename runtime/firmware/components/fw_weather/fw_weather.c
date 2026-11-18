#include "fw_weather.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WHITE 0xFFFF
#define GREY 0x8410
#define ORANGE 0xFD20
#define CLOCK_SET 1704067199   /* time() before 2024 counts as "not set" (FUN_4200ee88) */

static bool eq(const char *a, const char *b){ return a && b && strcmp(a, b) == 0; }

static void copy(char *out, size_t cap, const char *s){
    if (cap) snprintf(out, cap, "%s", s ? s : "");
}

void fw_weather_init(FwWeather *w){ memset(w, 0, sizeof *w); }

bool fw_weather_valid(const FwWeather *w){ return w && w->stn[0] && w->obs; }

/* ------------------------------------------------------------ record */

static bool alnum(char c){ return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

/* FUN_420e0698: every byte outside 0x20..0x7E becomes '?'. */
static void printable(char *out, size_t cap, const char *in){
    size_t i;
    copy(out, cap, in);
    for (i = 0; out[i]; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7E) out[i] = '?';
}

void fw_weather_set_str(FwWeather *w, const char *key, const char *v){
    static const char *const conds[] = {"clear", "partly_cloudy", "cloudy", "overcast", "fog", "rain", "snow",
                                        "thunder", NULL};
    if (!v) v = "";
    if (eq(key, "stn")) {                              /* FUN_42006c0c */
        size_t n = strlen(v);
        copy(w->stn, sizeof w->stn, (n == 3 || n == 4) && alnum(v[0]) ? v : "");
    } else if (eq(key, "name")) {
        printable(w->name, sizeof w->name, v);
    } else if (eq(key, "raw")) {                       /* FUN_42006d24 */
        printable(w->raw, sizeof w->raw, v);
        if (strlen(v) > sizeof w->raw - 1) {
            char *sp = strrchr(w->raw, ' ');
            if (sp) *sp = 0;
        }
    } else if (eq(key, "cat")) {                       /* FUN_4200eadc */
        w->cat = eq(v, "VFR") ? FW_WX_VFR : eq(v, "MVFR") ? FW_WX_MVFR : eq(v, "IFR") ? FW_WX_IFR
               : eq(v, "LIFR") ? FW_WX_LIFR : FW_WX_CAT_NONE;
    } else if (eq(key, "cond")) {                      /* FUN_42006cfc */
        int i;
        w->cond = FW_WX_COND_NONE;
        if (strlen(v) <= 15)
            for (i = 0; conds[i]; ++i)
                if (eq(v, conds[i])) { w->cond = (uint8_t)(i + 1); break; }
    }
}

static bool whole(double v, double lo, double hi){ return v == v && v == floor(v) && v >= lo && v <= hi; }

/* FUN_4200712c and friends: negative or NaN -> unset; rounded; capped. */
static bool u16_of(double v, uint16_t *out){
    if (!(v == v) || v < 0) return false;
    *out = v >= 65535.0 ? 65535 : (uint16_t)floor(v + 0.5);
    return true;
}

static uint32_t epoch_of(double v){ return v >= 1 && v <= 4294967295.0 ? (uint32_t)v : 0; }

void fw_weather_set_num(FwWeather *w, const char *key, double v){
    if (eq(key, "obs")) w->obs = epoch_of(v);
    else if (eq(key, "utc_at")) w->utc_at = epoch_of(v);
    else if (eq(key, "wdir")) { w->has_wdir = whole(v, 0, 360); if (w->has_wdir) w->wdir = (uint16_t)v; }
    else if (eq(key, "wspd")) w->has_wspd = u16_of(v, &w->wspd);
    else if (eq(key, "wgst")) w->has_wgst = u16_of(v, &w->wgst);
    else if (eq(key, "vis_m")) w->has_vis = u16_of(v, &w->vis_m);
    else if (eq(key, "ceil_ft")) w->has_ceil = u16_of(v, &w->ceil_ft);
    else if (eq(key, "temp_c")) { w->has_temp = whole(v, -99, 99); if (w->has_temp) w->temp_c = (int8_t)v; }
    else if (eq(key, "utc_off")) { w->has_utc_off = whole(v, -720, 840); if (w->has_utc_off) w->utc_off = (int16_t)v; }
    else if (eq(key, "utc_next")) { w->has_utc_next = whole(v, -720, 840); if (w->has_utc_next) w->utc_next = (int16_t)v; }
}

void fw_weather_set_bool(FwWeather *w, const char *key, bool v){
    if (eq(key, "auto")) w->is_auto = v;
    else if (eq(key, "night")) w->night = v;
    else if (eq(key, "wvar")) w->wvar = v;
    else if (eq(key, "vis_plus")) w->vis_plus = v;
}

/* ------------------------------------------------------------ settings */

FwWeatherLayout fw_weather_layout_parse(const char *s){
    return eq(s, "simple") ? FW_WX_SIMPLE : eq(s, "technical") ? FW_WX_TECHNICAL : FW_WX_SUMMARY;
}
uint8_t fw_weather_temp_unit(const char *s){ return eq(s, "fahrenheit") ? 1 : 0; }
uint8_t fw_weather_wind_unit(const char *s){ return eq(s, "mph") ? 1 : eq(s, "kmh") ? 2 : eq(s, "mps") ? 3 : 0; }
uint8_t fw_weather_vis_unit(const char *s){ return eq(s, "miles") ? 1 : eq(s, "kilometers") ? 2 : 0; }
uint8_t fw_weather_ceil_unit(const char *s){ return eq(s, "meters") ? 1 : 0; }

/* ------------------------------------------------------------ text */

long fw_weather_age(const FwWeather *w, int64_t now){
    if (now <= CLOCK_SET) return -1;
    if ((int64_t)w->obs > now) return 0;
    return (long)(now - (int64_t)w->obs);
}

static void age_text(long age, bool ago, char *out, size_t cap){
    snprintf(out, cap, ago ? "%ldh%02ldm ago" : "%ldh%02ldm", age / 3600, (age / 60) % 60);
}

bool fw_weather_wind(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap){
    static const double factor[] = {1.0, 1.150779, 1.852, 0.514444};
    static const char *const unit[] = {"kt", "mph", "km/h", "m/s"};
    char dir[12] = "", gust[12] = "";
    int u = st->wind_unit < 4 ? st->wind_unit : 0;
    long spd;
    copy(out, cap, "");
    if (!w->has_wspd) return false;
    if (w->wspd == 0) { copy(out, cap, style == 0 ? "Wind calm" : style == 1 ? "Calm" : "calm"); return true; }
    if (w->wvar) copy(dir, sizeof dir, style == 0 ? "variable" : "VRB");
    else if (w->has_wdir) snprintf(dir, sizeof dir, "%03u\xC2\xB0", (unsigned)w->wdir);
    spd = lround(w->wspd * factor[u]);
    if (w->has_wgst) snprintf(gust, sizeof gust, style == 0 ? " G%ld" : "G%ld", lround(w->wgst * factor[u]));
    if (style != 0) snprintf(out, cap, "%s%s%ld%s%s", dir, *dir ? "/" : "", spd, gust, unit[u]);
    else if (*dir) snprintf(out, cap, "Wind %s at %ld%s %s", dir, spd, gust, unit[u]);
    else snprintf(out, cap, "Wind %ld%s %s", spd, gust, unit[u]);
    return true;
}

bool fw_weather_vis(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap){
    static const struct { double mi; const char *text; } frac[] = {
        {0, "0"}, {0.25, "1/4"}, {0.5, "1/2"}, {0.75, "3/4"}, {1, "1"}, {1.5, "1 1/2"}, {2, "2"}, {2.5, "2 1/2"},
    };
    char num[16];
    bool sm;
    copy(out, cap, "");
    if (!w->has_vis) return false;
    sm = st->vis_unit == 1 || (st->vis_unit == 0 && (w->stn[0] == 'K' || w->stn[0] == 'P'));
    if (w->vis_plus) {
        copy(num, sizeof num, style == 1 ? "10" : "10+");
    } else if (sm) {
        double mi = w->vis_m / 1609.344;
        if (mi < 2.75) {
            int best = 0;
            size_t i;
            for (i = 1; i < sizeof frac / sizeof frac[0]; ++i)
                if (fabs(mi - frac[i].mi) < fabs(mi - frac[best].mi)) best = (int)i;
            copy(num, sizeof num, frac[best].text);
        } else {
            snprintf(num, sizeof num, "%ld", lround(mi));
        }
    } else {
        unsigned m = w->vis_m;
        if (m + 50 > 4999) snprintf(num, sizeof num, "%u", (m + 500) / 1000);
        else {
            unsigned t = (m + 50) / 100;
            if (t % 10) snprintf(num, sizeof num, "%u.%u", (m + 50) / 1000, t % 10);
            else snprintf(num, sizeof num, "%u", (m + 50) / 1000);
        }
    }
    snprintf(out, cap, style == 1 ? "%s%s" : "%s %s", num, sm ? "SM" : "km");
    return true;
}

static void thousands(long v, char *out, size_t cap){
    char digits[16], buf[24];
    size_t n, i, k = 0;
    snprintf(digits, sizeof digits, "%ld", v < 0 ? -v : v);
    n = strlen(digits);
    if (v < 0) buf[k++] = '-';
    for (i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0) buf[k++] = ',';
        buf[k++] = digits[i];
    }
    buf[k] = 0;
    if (k >= cap) k = cap ? cap - 1 : 0;     /* stock keeps 15 characters at most */
    if (cap) { memcpy(out, buf, k); out[k] = 0; }
}

bool fw_weather_ceil(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap){
    long v;
    const char *unit = st->ceil_unit == 1 ? "m" : "ft";
    char num[16];
    copy(out, cap, "");
    if (!w->has_ceil) return false;
    v = st->ceil_unit == 1 ? lround(w->ceil_ft * 0.3048) : (long)w->ceil_ft;
    if (style == 1) { snprintf(out, cap, "Ceil %ld%s", v, unit); return true; }
    thousands(v, num, sizeof num);
    snprintf(out, cap, style == 0 ? "Ceiling %s %s" : "%s %s", num, unit);
    return true;
}

bool fw_weather_temp(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap){
    long v;
    copy(out, cap, "");
    if (!w->has_temp) return false;
    v = st->temp_unit == 1 ? lround(w->temp_c * 9.0 / 5.0 + 32.0) : (long)w->temp_c;
    snprintf(out, cap, style == 0 ? "Temp %ld\xC2\xB0%s" : "%ld\xC2\xB0%s", v, st->temp_unit == 1 ? "F" : "C");
    return true;
}

void fw_weather_time(const FwWeather *w, const FwWeatherSettings *st, int64_t now, char *out, size_t cap){
    FwZone z = st->zone;
    FwTime t;
    int h;
    copy(out, cap, "");
    if (now <= CLOCK_SET) return;
    if (w->has_utc_off) {
        z.standard_offset_min = w->has_utc_next && w->utc_at && now >= (int64_t)w->utc_at ? w->utc_next : w->utc_off;
        z.dst_rule = FW_DST_NONE;
    }
    if (!fw_local_time(now, z, &t)) return;
    if (st->is_24h) {
        if (st->show_seconds) snprintf(out, cap, "%02d:%02d:%02d", t.hour, t.min, t.sec);
        else snprintf(out, cap, "%02d:%02d", t.hour, t.min);
        return;
    }
    h = t.hour % 12 ? t.hour % 12 : 12;
    if (st->show_seconds) snprintf(out, cap, "%d:%02d:%02d %s", h, t.min, t.sec, t.hour < 12 ? "AM" : "PM");
    else snprintf(out, cap, "%d:%02d %s", h, t.min, t.hour < 12 ? "AM" : "PM");
}

const char *fw_weather_cat_name(uint8_t cat){
    static const char *const n[] = {"---", "VFR", "MVFR", "IFR", "LIFR"};
    return cat <= FW_WX_LIFR ? n[cat] : n[0];
}

uint16_t fw_weather_cat_color(uint8_t cat){
    static const uint16_t c[] = {GREY, 0x07E0, 0x041F, 0xF800, 0xF81F};
    return cat <= FW_WX_LIFR ? c[cat] : GREY;
}

const char *fw_weather_icon_name(const FwWeather *w){
    switch (w->cond) {
    case FW_WX_CLEAR: return w->night ? "clear_night" : "clear_day";
    case FW_WX_PARTLY_CLOUDY: return w->night ? "partly_cloudy_night" : "partly_cloudy_day";
    case FW_WX_CLOUDY: return "cloudy";
    case FW_WX_OVERCAST: return "overcast";
    case FW_WX_FOG: return "fog";
    case FW_WX_RAIN: return "rain";
    case FW_WX_SNOW: return "snow";
    case FW_WX_THUNDER: return "thunder";
    default: return NULL;
    }
}

static const char *cond_text(uint8_t cond){
    static const char *const n[] = {"", "Clear", "Partly cloudy", "Cloudy", "Overcast", "Fog", "Rain", "Snow", "Thunder"};
    return cond <= FW_WX_THUNDER ? n[cond] : "";
}

/* ------------------------------------------------------------ frame */

#define DOT "\xE2\x97\x8F"          /* ● */
#define NEXT_PAGE "\xE2\x96\xB8"    /* ▸ */
#define COLS 21

static int cps(const char *s){ return fw_text_glyph_count(s); }

static FwWxRow *row_at(FwWxFrame *f, int i, int x, int y, int w, int size, bool center){
    FwWxRow *r = &f->row[i];
    if (f->nrow < i + 1) f->nrow = (uint8_t)(i + 1);
    r->x = (int16_t)x; r->y = (int16_t)y; r->w = (int16_t)w;
    r->max_size = (uint8_t)size; r->center = center;
    return r;
}

static void add(FwWxRow *r, const char *text, uint16_t color){
    if (!text || !*text || r->nseg >= FW_WX_SEGS) return;
    if (r->nseg && r->seg[r->nseg - 1].color == color &&
        strlen(r->seg[r->nseg - 1].text) + strlen(text) < sizeof r->seg[0].text) {
        strcat(r->seg[r->nseg - 1].text, text);
        return;
    }
    copy(r->seg[r->nseg].text, sizeof r->seg[0].text, text);
    r->seg[r->nseg++].color = color;
}

static void spaces(FwWxRow *r, int n, uint16_t color){
    char sp[COLS + 1];
    if (n <= 0) return;
    if (n > COLS) n = COLS;
    memset(sp, ' ', (size_t)n);
    sp[n] = 0;
    add(r, sp, color);
}

static int row_cps(const FwWxRow *r){
    int n = 0, i;
    for (i = 0; i < r->nseg; ++i) n += cps(r->seg[i].text);
    return n;
}

/* The parts a header's right side is built from (tables 0x3c0f219c, 0x3c0f2168, 0x3c0f2150). */
enum { P_STN = 1, P_AGE = 2, P_AGO = 4, P_DOT = 8, P_CAT = 16 };

static void right_part(FwWxRow *out, unsigned parts, const FwWeather *w, uint16_t stn_color, long age){
    uint16_t cc = fw_weather_cat_color(w->cat);
    char a[24];
    bool first = true;
#define SEP() do { if (!first) add(out, " ", WHITE); first = false; } while (0)
    if (parts & P_STN) { SEP(); add(out, w->stn, stn_color); }
    if (parts & P_AGE) { SEP(); age_text(age, (parts & P_AGO) != 0, a, sizeof a); add(out, a, ORANGE); }
    if (parts & P_DOT) { SEP(); add(out, DOT, cc); }
    if (parts & P_CAT) {
        if (!first) add(out, " ", (parts & P_DOT) ? cc : WHITE);
        first = false;
        add(out, fw_weather_cat_name(w->cat), cc);
    }
#undef SEP
}

/* Header row: the time at column 0, then the first variant that fits, ending at column 19. */
static void header(FwWxFrame *f, const FwWeather *w, const char *time, const unsigned *variants, int nvar,
                   uint16_t stn_color, long age){
    FwWxRow *r = row_at(f, 0, 1, 4, 126, 1, false);
    int tw = cps(time), budget = *time ? 19 - tw : 20, i;
    FwWxRow right;
    for (i = 0; i < nvar; ++i) {
        memset(&right, 0, sizeof right);
        right_part(&right, variants[i], w, stn_color, age);
        if (row_cps(&right) <= budget || i == nvar - 1) break;
    }
    add(r, time, WHITE);
    spaces(r, 20 - tw - row_cps(&right) > 1 ? 20 - tw - row_cps(&right) : 1, WHITE);
    for (i = 0; i < right.nseg; ++i) add(r, right.seg[i].text, right.seg[i].color);
}

static void build_simple(FwWxFrame *f, const FwWeather *w, const FwWeatherSettings *st, const char *time, long age){
    char temp[24], a[24];
    bool has_temp = fw_weather_temp(w, st, 1, temp, sizeof temp), old = age > FW_WX_AGE_SHOWN_S;
    const char *cands[3] = {w->name, w->stn, ""};
    FwWxRow *r;
    int i;
    age_text(age, true, a, sizeof a);
    add(row_at(f, 0, 40, 7, 86, 1, true), time, WHITE);
    r = row_at(f, 1, 40, 19, 86, 2, true);
    if (has_temp) add(r, temp, WHITE);
    else if (old) add(r, a, ORANGE);
    else add(r, cond_text(w->cond), WHITE);
    r = row_at(f, 2, 1, 49, 126, 1, true);
    for (i = 0; i < 3; ++i) {
        FwWxRow c;
        if (i < 2 && !*cands[i]) continue;
        memset(&c, 0, sizeof c);
        if (*cands[i]) { add(&c, cands[i], WHITE); add(&c, " ", WHITE); }
        if (has_temp && old) { add(&c, a, ORANGE); add(&c, " ", ORANGE); }
        add(&c, DOT " ", fw_weather_cat_color(w->cat));
        add(&c, fw_weather_cat_name(w->cat), fw_weather_cat_color(w->cat));
        if (row_cps(&c) <= COLS || i == 2) {
            int k;
            for (k = 0; k < c.nseg; ++k) add(r, c.seg[k].text, c.seg[k].color);
            break;
        }
    }
}

static void build_summary(FwWxFrame *f, const FwWeather *w, const FwWeatherSettings *st, const char *time, long age){
    static const unsigned variants[] = {P_STN | P_DOT | P_CAT, P_STN | P_CAT, P_STN | P_DOT, P_CAT};
    static const char *const labels[] = {"Wind ", "Vis  ", "Ceil ", "Temp "};
    char v[4][40];
    bool have[4];
    int i, n = 1;
    header(f, w, time, variants, 4, WHITE, age);
    for (i = 1; i < FW_WX_ROWS; ++i) row_at(f, i, 1, 4 + 12 * i, 126, 1, false);   /* every row, so a redraw clears it */
    have[0] = fw_weather_wind(w, st, 2, v[0], sizeof v[0]);
    have[1] = fw_weather_vis(w, st, 2, v[1], sizeof v[1]);
    have[2] = fw_weather_ceil(w, st, 2, v[2], sizeof v[2]);
    have[3] = fw_weather_temp(w, st, 2, v[3], sizeof v[3]);
    for (i = 0; i < 4; ++i) {
        FwWxRow *r;
        if (!have[i]) continue;
        r = row_at(f, n, 1, 4 + 12 * n, 126, 1, false);
        ++n;
        if (cps(v[i]) + 4 < COLS) add(r, labels[i], GREY);
        add(r, v[i], WHITE);
    }
    if (age > FW_WX_AGE_SHOWN_S) {
        char a[24];
        FwWxRow *r = row_at(f, 4, 1, 52, 126, 1, false);
        int used = row_cps(r), pad;
        age_text(age, true, a, sizeof a);
        pad = 20 - used - cps(a);
        if (used) spaces(r, pad > 1 ? pad : 1, WHITE);
        else spaces(r, pad, WHITE);
        add(r, a, ORANGE);
    }
}

/* FUN_420e099c: greedy word wrap; short = the last line of every page 3 columns shorter. */
static int wrap(const char *s, bool short_last, char lines[32][COLS + 1]){
    size_t len = strlen(s), pos = 0;
    int n = 0;
    while (pos < len && n < 32) {
        size_t cap = short_last && n % 4 == 3 ? COLS - 3 : COLS, take, i;
        while (pos < len && s[pos] == ' ') ++pos;
        if (pos >= len) break;
        if (len - pos <= cap) take = len - pos;
        else {
            take = 0;
            for (i = cap; i > 0; --i)
                if (s[pos + i] == ' ') { take = i; break; }
            if (!take) take = cap;
        }
        memcpy(lines[n], s + pos, take);
        lines[n][take] = 0;
        while (take && lines[n][take - 1] == ' ') lines[n][--take] = 0;
        ++n;
        pos += take;
    }
    return n;
}

static void build_technical(FwWxFrame *f, const FwWeather *w, const char *time, long age, uint32_t page_ms){
    static const unsigned fresh[] = {P_STN | P_CAT, P_STN | P_DOT, P_CAT};
    static const unsigned stale[] = {P_STN | P_AGE | P_AGO | P_CAT, P_STN | P_AGE | P_CAT, P_STN | P_AGE | P_DOT,
                                     P_STN | P_DOT, P_AGE | P_AGO | P_CAT, P_CAT};
    char body[257] = "", tok[257], lines[32][COLS + 1];
    const char *p = w->raw;
    bool first = true, old = age > FW_WX_AGE_SHOWN_S;
    int n, pages, page, i;
    if (old) header(f, w, time, stale, 6, ORANGE, age);
    else header(f, w, time, fresh, 3, WHITE, age);
    while (*p) {                                 /* FUN_4200fa70 */
        size_t k = 0;
        while (*p == ' ') ++p;
        if (!*p) break;
        while (p[k] && p[k] != ' ') ++k;
        memcpy(tok, p, k); tok[k] = 0;
        p += k;
        if (first && eq(tok, w->stn)) { first = false; continue; }
        first = false;
        if (eq(tok, "RMK")) break;
        if (strlen(body) + k + 2 < sizeof body) {
            if (*body) strcat(body, " ");
            strcat(body, tok);
        }
    }
    n = wrap(body, false, lines);
    if (n > 4) n = wrap(body, true, lines);
    pages = n ? (n + 3) / 4 : 1;
    page = (int)((page_ms / FW_WX_PAGE_MS) % (uint32_t)pages);
    for (i = 0; i < 4; ++i) {
        int li = page * 4 + i;
        FwWxRow *r = row_at(f, 1 + i, 1, 16 + 12 * i, 126, 1, false);
        if (li >= n) continue;
        add(r, lines[li], WHITE);
        if (i == 3 && page < pages - 1) {
            int pad = 19 - cps(lines[li]);
            spaces(r, pad > 1 ? pad : 1, WHITE);
            add(r, NEXT_PAGE, WHITE);
        }
    }
}

void fw_weather_frame(const FwWeather *w, const FwWeatherSettings *st, int64_t now, uint32_t page_ms, FwWxFrame *out){
    char time[24];
    const char *icon;
    long age = fw_weather_age(w, now);
    memset(out, 0, sizeof *out);
    out->layout = st->layout;
    fw_weather_time(w, st, now, time, sizeof time);
    switch (st->layout) {
    case FW_WX_SIMPLE:
        icon = fw_weather_icon_name(w);
        copy(out->icon, sizeof out->icon, icon ? icon : "");
        build_simple(out, w, st, time, age);
        break;
    case FW_WX_TECHNICAL: build_technical(out, w, time, age, page_ms); break;
    default: build_summary(out, w, st, time, age); break;
    }
}

/* ------------------------------------------------------------ drawing */

static FwRgb rgb(uint16_t c){
    FwRgb o;
    uint8_t r5 = (uint8_t)(c >> 11), g6 = (uint8_t)((c >> 5) & 63), b5 = (uint8_t)(c & 31);
    o.r = (uint8_t)(r5 << 3 | r5 >> 2);
    o.g = (uint8_t)(g6 << 2 | g6 >> 4);
    o.b = (uint8_t)(b5 << 3 | b5 >> 2);
    return o;
}

static void draw_row(const FwSurface *s, const FwWxRow *r){
    static const FwRgb black = {0, 0, 0};
    int n = row_cps(r), size = r->max_size ? r->max_size : 1, x, end, i;
    while (size > 1 && n * 6 * size > r->w) --size;        /* FUN_4200fbc0 */
    x = r->x;
    if (r->center && n * 6 * size < r->w) x += (r->w - n * 6 * size) / 2;
    if (x > r->x) fw_fill_rect(s, r->x, r->y, x - r->x, 8 * r->max_size, black);
    for (i = 0; i < r->nseg; ++i) x = fw_text_draw_bg(s, x, r->y, r->seg[i].text, rgb(r->seg[i].color), black, size);
    end = r->x + r->w;
    if (x < end) fw_fill_rect(s, x, r->y, end - x, 8 * r->max_size, black);
    if (size < r->max_size) fw_fill_rect(s, r->x, r->y + 8 * size, r->w, 8 * (r->max_size - size), black);
}

uint32_t fw_weather_row_hash(const FwWxRow *r){
    uint32_t h = 0x811c9dc5u;
    int i;
    const uint8_t *p;
    size_t k;
#define MIX(ptr, n) for (p = (const uint8_t *)(ptr), k = (n); k--; ) { h ^= *p++; h *= 0x01000193u; }
    MIX(&r->x, sizeof r->x); MIX(&r->y, sizeof r->y); MIX(&r->w, sizeof r->w);
    MIX(&r->max_size, 1); MIX(&r->center, 1); MIX(&r->nseg, 1);
    for (i = 0; i < r->nseg; ++i) { MIX(r->seg[i].text, strlen(r->seg[i].text) + 1); MIX(&r->seg[i].color, 2); }
#undef MIX
    return h;
}

void fw_weather_draw_rows(const FwSurface *s, const FwWxFrame *f, uint32_t *hashes){
    int i;
    for (i = 0; i < f->nrow && i < FW_WX_ROWS; ++i) {
        uint32_t h;
        if (!f->row[i].w) continue;
        h = fw_weather_row_hash(&f->row[i]);
        if (hashes && hashes[i] == h) continue;
        draw_row(s, &f->row[i]);
        if (hashes) hashes[i] = h;
    }
}

void fw_weather_draw(const FwSurface *s, const FwWxFrame *f, bool icon_drawn){
    if (!icon_drawn) {
        fw_clear(s);
        if (f->icon[0]) fw_weather_icon_draw(s, 4, 6, f->icon);
    }
    fw_weather_draw_rows(s, f, NULL);
}

/* ------------------------------------------------------------ owned icons
 * Drawn here, not copied: a sun, a moon, clouds, rain, snow, fog and a bolt,
 * in 32x32, for walls whose filesystem has no /images/weather/32/ icons. */

typedef struct { const FwSurface *s; int ox, oy; } Pen;

static void px(const Pen *p, int x, int y, FwRgb c){
    if (x < 0 || y < 0 || x >= 32 || y >= 32) return;
    x += p->ox; y += p->oy;
    if (x >= 0 && y >= 0 && x < p->s->width && y < p->s->height) p->s->pixel(p->s->ctx, x, y, c);
}

static void disc(const Pen *p, int cx, int cy, int r, FwRgb c){
    int x, y;
    for (y = -r; y <= r; ++y)
        for (x = -r; x <= r; ++x)
            if (x * x + y * y <= r * r + r / 2) px(p, cx + x, cy + y, c);
}

static void line(const Pen *p, int x0, int y0, int x1, int y1, FwRgb c){
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        int e2;
        px(p, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void sun(const Pen *p, int cx, int cy, int r){
    FwRgb y = {255, 200, 0};
    int k;
    static const int dir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    disc(p, cx, cy, r, y);
    for (k = 0; k < 8; ++k) {
        int d = dir[k][0] && dir[k][1] ? r + 1 : r + 2, l = dir[k][0] && dir[k][1] ? 2 : 3;
        line(p, cx + dir[k][0] * d, cy + dir[k][1] * d, cx + dir[k][0] * (d + l), cy + dir[k][1] * (d + l), y);
    }
}

static void moon(const Pen *p, int cx, int cy, int r){
    FwRgb m = {230, 230, 190}, k = {0, 0, 0};
    disc(p, cx, cy, r, m);
    disc(p, cx + r / 2 + 1, cy - r / 2, r - 1, k);
}

static void cloud(const Pen *p, int ox, int oy, FwRgb c){   /* 26 x 17 */
    int x;
    disc(p, ox + 7, oy + 10, 6, c);
    disc(p, ox + 14, oy + 7, 7, c);
    disc(p, ox + 21, oy + 10, 5, c);
    for (x = ox + 2; x <= ox + 25; ++x) line(p, x, oy + 10, x, oy + 15, c);
}

void fw_weather_icon_draw(const FwSurface *s, int x, int y, const char *name){
    Pen p = {s, x, y};
    FwRgb light = {220, 220, 220}, mid = {150, 150, 160}, dark = {90, 90, 100}, blue = {60, 140, 255},
          white = {255, 255, 255}, bolt = {255, 210, 0};
    int i;
    if (!s || !s->pixel || !name) return;
    if (eq(name, "clear_day")) sun(&p, 16, 16, 7);
    else if (eq(name, "clear_night")) moon(&p, 16, 16, 10);
    else if (eq(name, "partly_cloudy_day")) { sun(&p, 11, 10, 5); cloud(&p, 5, 13, light); }
    else if (eq(name, "partly_cloudy_night")) { moon(&p, 11, 10, 7); cloud(&p, 5, 13, light); }
    else if (eq(name, "cloudy")) { cloud(&p, 5, 3, mid); cloud(&p, 1, 11, light); }
    else if (eq(name, "overcast")) { cloud(&p, 5, 3, dark); cloud(&p, 1, 11, mid); }
    else if (eq(name, "fog")) {
        cloud(&p, 3, 1, mid);
        for (i = 0; i < 3; ++i) line(&p, 3 + 3 * (i & 1), 21 + 4 * i, 28 - 3 * (i & 1), 21 + 4 * i, light);
    } else if (eq(name, "rain")) {
        cloud(&p, 3, 1, light);
        for (i = 0; i < 5; ++i) line(&p, 8 + 4 * i, 20 + (i & 1) * 3, 6 + 4 * i, 26 + (i & 1) * 3, blue);
    } else if (eq(name, "snow")) {
        cloud(&p, 3, 1, light);
        for (i = 0; i < 5; ++i) {
            int cx = 6 + 5 * i, cy = 22 + (i & 1) * 5;
            px(&p, cx, cy, white); px(&p, cx - 1, cy, white); px(&p, cx + 1, cy, white);
            px(&p, cx, cy - 1, white); px(&p, cx, cy + 1, white);
        }
    } else if (eq(name, "thunder")) {
        cloud(&p, 3, 1, dark);
        line(&p, 18, 17, 14, 24, bolt); line(&p, 19, 17, 15, 24, bolt);
        line(&p, 14, 24, 19, 24, bolt);
        line(&p, 19, 24, 15, 31, bolt); line(&p, 18, 25, 14, 31, bolt);
    }
}

/* ------------------------------------------------------------ owned: aviationweather.gov */

bool fw_weather_is_night(double lat, double lon, int64_t t){
    /* NOAA's simple solar position: day of year, fractional hour, declination
     * and the equation of time; good to a fraction of a degree. */
    const double pi = 3.14159265358979323846, rad = pi / 180.0;
    double days = (double)t / 86400.0, frac_day = days - floor(days), g, decl, eqt, ha, elev;
    double doy = fmod(days, 365.2422);
    g = 2.0 * pi / 365.0 * (doy - 1.0 + (frac_day * 24.0 - 12.0) / 24.0);
    decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g)
         - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    eqt = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    ha = ((frac_day * 1440.0 + eqt + 4.0 * lon) / 4.0 - 180.0) * rad;
    elev = asin(sin(lat * rad) * sin(decl) + cos(lat * rad) * cos(decl) * cos(ha)) / rad;
    return elev < -0.833;
}

/* A weather code anywhere in wxString ("-TSRA BR" has TS, RA and BR). */
static bool has_wx(const char *wx, const char *code){ return wx && strstr(wx, code) != NULL; }

void fw_weather_from_awc(const FwAwcMetar *m, FwWeather *w){
    char name[64];
    int i, worst = 0;
    double ceil = -1;
    fw_weather_init(w);
    fw_weather_set_str(w, "stn", m->icao);
    copy(name, sizeof name, m->name);
    {   /* "Ft Lauderdale-Hollywood Intl, FL, US" -> before the first comma, then
         * whole words that fit the record's 24 bytes */
        char *comma = strchr(name, ',');
        size_t n;
        if (comma) *comma = 0;
        while ((n = strlen(name)) > sizeof w->name - 1) {
            char *sp = strrchr(name, ' ');
            if (!sp) { name[sizeof w->name - 1] = 0; break; }
            *sp = 0;
        }
        while ((n = strlen(name)) && name[n - 1] == ' ') name[n - 1] = 0;
    }
    fw_weather_set_str(w, "name", name);
    fw_weather_set_str(w, "raw", m->raw);
    fw_weather_set_str(w, "cat", m->flt_cat);
    fw_weather_set_num(w, "obs", m->obs);
    fw_weather_set_num(w, "temp_c", m->temp == m->temp ? floor(m->temp + 0.5) : m->temp);
    if (m->wdir_vrb) fw_weather_set_bool(w, "wvar", true);
    else fw_weather_set_num(w, "wdir", m->wdir == m->wdir ? floor(m->wdir + 0.5) : m->wdir);
    fw_weather_set_num(w, "wspd", m->wspd);
    fw_weather_set_num(w, "wgst", m->wgst);
    if (m->visib_plus) {
        fw_weather_set_bool(w, "vis_plus", true);
        fw_weather_set_num(w, "vis_m", 16093);
    } else if (m->visib == m->visib) {
        fw_weather_set_num(w, "vis_m", m->visib * 1609.344);
    }
    for (i = 0; i < m->ncloud && i < FW_AWC_CLOUDS; ++i) {
        const char *c = m->cover[i] ? m->cover[i] : "";
        int level = eq(c, "OVC") || eq(c, "OVX") ? 4 : eq(c, "BKN") ? 3 : eq(c, "SCT") ? 2 : eq(c, "FEW") ? 1 : 0;
        if (level > worst) worst = level;
        if (level >= 3 && m->base[i] == m->base[i] && (ceil < 0 || m->base[i] < ceil)) ceil = m->base[i];
    }
    if (ceil >= 0) fw_weather_set_num(w, "ceil_ft", ceil);
    if (has_wx(m->wx, "TS")) w->cond = FW_WX_THUNDER;
    else if (has_wx(m->wx, "SN") || has_wx(m->wx, "SG") || has_wx(m->wx, "PL") || has_wx(m->wx, "GS")) w->cond = FW_WX_SNOW;
    else if (has_wx(m->wx, "RA") || has_wx(m->wx, "DZ") || has_wx(m->wx, "UP")) w->cond = FW_WX_RAIN;
    else if (has_wx(m->wx, "FG") || has_wx(m->wx, "BR") || has_wx(m->wx, "HZ") || has_wx(m->wx, "FU")) w->cond = FW_WX_FOG;
    else w->cond = worst == 4 ? FW_WX_OVERCAST : worst == 3 ? FW_WX_CLOUDY : worst == 2 ? FW_WX_PARTLY_CLOUDY : FW_WX_CLEAR;
    if (m->lat == m->lat && m->lon == m->lon && w->obs)
        w->night = fw_weather_is_night(m->lat, m->lon, (int64_t)w->obs);
}
