#include "fw_flight.h"
#include "fw_text.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Unit factors, bit-exact copies of the vendor's literal-pool doubles. */
#define K_FEET  0x1.a3f290abb44e5p+1    /* 3.28084 */
#define K_KNOTS 0x1.f19f7f8ca8199p+0    /* 1.94384 */
#define K_MPH   0x1.1e540cc78e9f7p+1    /* 2.23694 */
#define K_KPH   0x1.ccccccccccccdp+1    /* 3.6 */
#define K_FPM   0x1.89b367a0f9097p+7    /* 196.8504 */
#define K_MILES 0x1.45c7p-11f           /* 0.000621371 (float) */

static const char NA[] = "N/A";

void fw_flight_init(FwFlight *f){
    memset(f, 0, sizeof *f);
    f->alt_b = f->alt_g = f->vel = f->trk = f->vr = NAN;
    f->prog = f->dist = NAN;
}

/* NAN test without math.h's isnan macro (whose MinGW form warns on doubles). */
static bool nan_d(double v){ return v != v; }
static bool nan_f(float v){ return v != v; }

static bool eq(const char *a, const char *b){ return a && b && strcmp(a, b) == 0; }

void fw_units_parse(FwUnits *u, const char *alt, const char *vel, const char *vr, const char *trk,
                    const char *dist){
    u->alt = eq(alt, "feet") ? FW_ALT_FEET : eq(alt, "kilometers") ? FW_ALT_KILOMETERS
           : eq(alt, "nautical_miles") ? FW_ALT_NAUTICAL_MILES : FW_ALT_METERS;
    u->vel = eq(vel, "mps") ? FW_VEL_MPS : eq(vel, "knots") ? FW_VEL_KNOTS : eq(vel, "mph") ? FW_VEL_MPH
           : eq(vel, "mach") ? FW_VEL_MACH : FW_VEL_KMH;
    u->vr = eq(vr, "mps") ? FW_VR_MPS : eq(vr, "fps") ? FW_VR_FPS : eq(vr, "fpm") ? FW_VR_FPM
          : eq(vr, "mph") ? FW_VR_MPH : FW_VR_KMH;
    u->trk_cardinal = eq(trk, "cardinal");
    u->dist = eq(dist, "kilometers") ? FW_DIST_KILOMETERS : eq(dist, "miles") ? FW_DIST_MILES
            : eq(dist, "nautical_miles") ? FW_DIST_NAUTICAL_MILES : eq(dist, "feet") ? FW_DIST_FEET
            : FW_DIST_METERS;
}

FwTitle fw_title_parse(const char *t){
    return eq(t, "registration") ? FW_TITLE_REGISTRATION : eq(t, "callsign") ? FW_TITLE_CALLSIGN
                                                                             : FW_TITLE_AIRLINE;
}

/* ------------------------------------------------------------ strings */

static void copy(char *out, size_t cap, const char *s){
    if (cap) snprintf(out, cap, "%s", s ? s : "");
}

/* The vendor's replace-all (FUN_4203dbd0): left to right, never rescanning
 * inserted text; nothing happens when the text or the pattern is empty. */
static void replace_all(char *buf, size_t cap, const char *pat, const char *rep){
    char out[FW_FLIGHT_TEXT_MAX * 2];
    size_t plen = strlen(pat), rlen = strlen(rep), n = 0;
    const char *p = buf, *hit;
    if (!*buf || !plen || !strstr(buf, pat)) return;
    while ((hit = strstr(p, pat)) != NULL) {
        size_t lead = (size_t)(hit - p);
        if (n + lead + rlen >= sizeof out) break;
        memcpy(out + n, p, lead); n += lead;
        memcpy(out + n, rep, rlen); n += rlen;
        p = hit + plen;
    }
    {
        size_t rest = strlen(p);
        if (n + rest >= sizeof out) rest = sizeof out - 1 - n;
        memcpy(out + n, p, rest); n += rest;
    }
    out[n] = 0;
    copy(buf, cap, out);
}

void fw_flight_clean(const char *in, char *out, size_t cap){
    copy(out, cap, in);
    replace_all(out, cap, "\xE2\x80\x93", "-");          /* – */
    replace_all(out, cap, "\xE2\x80\x94", "-");          /* — */
    replace_all(out, cap, "\xE2\x80\x98", "'");          /* ‘ */
    replace_all(out, cap, "\xE2\x80\x99", "'");          /* ’ */
    replace_all(out, cap, "\xE2\x80\x9C", "\"");         /* “ */
    replace_all(out, cap, "\xE2\x80\x9D", "\"");         /* ” */
    replace_all(out, cap, "\xE2\x80\xA6", "...");        /* … */
    replace_all(out, cap, "\xC2\xA0", " ");              /* NBSP */
    replace_all(out, cap, "\xC2\xB0", "\xC2\xB0");       /* ° (stock replaces it with itself) */
}

/* Code points as stock counts them (FUN_4201bb44 with malformed bytes counted). */
static int code_points(const char *s){
    const char *p = s;
    int n = 0;
    while (fw_text_next_codepoint(&p, NULL) != FW_TEXT_END) ++n;
    return n;
}

/* Byte length of the first n code points (FUN_4200a868). */
static size_t prefix_bytes(const char *s, int n){
    const char *p = s;
    while (n-- > 0 && fw_text_next_codepoint(&p, NULL) != FW_TEXT_END) {}
    return (size_t)(p - s);
}

/* ------------------------------------------------------------ names */

static void cleaned_or_na(const char *s, char *out, size_t cap){
    if (*s) fw_flight_clean(s, out, cap);
    else copy(out, cap, NA);
}

static void callsign(const FwFlight *f, char *out, size_t cap){ cleaned_or_na(f->cs, out, cap); }

static void registration(const FwFlight *f, char *out, size_t cap){
    if (*f->reg) copy(out, cap, f->reg);
    else callsign(f, out, cap);
}

static void title(const FwFlightSettings *s, const FwFlight *f, char *out, size_t cap){
    const char *pick;
    if (s->title == FW_TITLE_REGISTRATION) { registration(f, out, cap); return; }
    if (s->title == FW_TITLE_CALLSIGN) { callsign(f, out, cap); return; }
    pick = *f->al_name ? f->al_name : *f->cs ? f->cs : *f->al_op ? f->al_op : *f->al_paint ? f->al_paint
         : f->al_code;
    cleaned_or_na(pick, out, cap);
}

static void upper(const char *in, char *out, size_t cap){
    size_t i;
    copy(out, cap, in);
    for (i = 0; out[i]; ++i)
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 'a' + 'A');
}

static bool anchored(const FwFlightSettings *s, const char *code){
    char a[16], b[16];
    int i;
    if (!*code || !s->anchors || s->anchor_count <= 0) return false;
    upper(code, a, sizeof a);
    for (i = 0; i < s->anchor_count; ++i) {
        upper(s->anchors[i] ? s->anchors[i] : "", b, sizeof b);
        if (strcmp(a, b) == 0) return true;
    }
    return false;
}

static const char *unknown(const FwFlightSettings *s, const char *text){
    return s->mini && (eq(s->layout, "DEFAULT") || eq(s->layout, "NO_LOGO")) ? text : NA;
}

static void origin(const FwFlightSettings *s, const FwFlight *f, char *out, size_t cap){
    if (anchored(s, f->dest_icao) || anchored(s, f->dest_iata)) copy(out, cap, "Arriving from");
    else if (anchored(s, f->orig_icao) || anchored(s, f->orig_iata)) copy(out, cap, "Departing to");
    else if (*f->orig) fw_flight_clean(f->orig, out, cap);
    else copy(out, cap, unknown(s, "Unknown origin"));
}

static void destination(const FwFlightSettings *s, const FwFlight *f, char *out, size_t cap){
    if (anchored(s, f->dest_icao) || anchored(s, f->dest_iata)) {
        if (*f->orig) fw_flight_clean(f->orig, out, cap);
        else copy(out, cap, unknown(s, "Unknown origin"));
    } else if (*f->dest) fw_flight_clean(f->dest, out, cap);
    else copy(out, cap, unknown(s, "Unknown destination"));
}

/* ------------------------------------------------------------ numbers */

static bool invalid(float v){ return nan_f(v) || fabsf(v) > FLT_MAX; }

/* FUN_42007b4c: 999, 5.3k, 35k. */
static void compact(long n, const char *unit, char *out, size_t cap){
    if (n >= -9999 && n <= 9999) {
        if (n >= -999 && n <= 999) snprintf(out, cap, "%ld%s", n, unit);
        else snprintf(out, cap, "%.1fk%s", (double)(float)((double)n / 1000.0), unit);
    } else {
        snprintf(out, cap, "%ldk%s", n / 1000, unit);
    }
}

static void altitude(const FwFlightSettings *s, double v, char *out, size_t cap){
    float f = (float)v;
    if (invalid(f)) { copy(out, cap, NA); return; }
    switch (s->units.alt) {
    case FW_ALT_KILOMETERS: compact(lround((double)f / 1000.0), "km", out, cap); break;
    case FW_ALT_NAUTICAL_MILES: compact(lround((double)f / 1852.0), "NM", out, cap); break;
    case FW_ALT_FEET: compact(lround((double)f * K_FEET), "ft", out, cap); break;
    default: compact(lroundf(f), "m", out, cap); break;
    }
}

static void speed(const FwFlightSettings *s, double v, char *out, size_t cap){
    float f = (float)v;
    if (invalid(f)) { copy(out, cap, NA); return; }
    switch (s->units.vel) {
    case FW_VEL_KNOTS: compact(lround((double)f * K_KNOTS), "kts", out, cap); break;
    case FW_VEL_MPS: compact(lroundf(f), "m/s", out, cap); break;
    case FW_VEL_MPH: compact(lround((double)f * K_MPH), "mph", out, cap); break;
    case FW_VEL_MACH: snprintf(out, cap < 16 ? cap : 16, "M%.2f", (double)(f / 343.0f)); break;
    default: compact(lround((double)f * K_KPH), "kph", out, cap); break;
    }
}

static void vertical(const FwFlightSettings *s, double v, char *out, size_t cap){
    float f = (float)v;
    char body[32];
    long n;
    const char *unit;
    if (invalid(f)) { copy(out, cap, NA); return; }
    switch (s->units.vr) {
    case FW_VR_FPS: n = lroundf(fabsf((float)((double)f * K_FEET))); unit = "ft/s"; break;
    case FW_VR_MPS: n = lroundf(fabsf(f)); unit = "m/s"; break;
    case FW_VR_FPM: n = lroundf(fabsf((float)((double)f * K_FPM))); unit = "fpm"; break;
    case FW_VR_MPH: n = lroundf(fabsf((float)((double)f * K_MPH))); unit = "mph"; break;
    default: n = lroundf(fabsf((float)((double)f * K_KPH))); unit = "kph"; break;
    }
    compact(n, unit, body, sizeof body);
    snprintf(out, cap, "%s%s", (double)f > 0.1 ? "+" : (double)f < -0.1 ? "-" : "", body);
}

static void heading(const FwFlightSettings *s, double v, char *out, size_t cap){
    static const char *const names[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    float f = (float)v;
    long r;
    unsigned deg;
    if (invalid(f)) { copy(out, cap, NA); return; }
    r = lroundf(f);
    deg = (unsigned)(r % 360 + 360) % 360u;
    if (s->units.trk_cardinal) copy(out, cap, names[((deg + 11) / 22) & 15]);
    else snprintf(out, cap < 16 ? cap : 16, "%udeg", deg);
}

/* FUN_4200795c */
static void small_distance(float v, const char *unit, char *out, size_t cap){
    float a = 0.0f <= v ? v : 0.0f - v;
    if (a >= 100.0f) snprintf(out, cap, "%ld%s", lroundf(v), unit);
    else snprintf(out, cap, "%.1f%s", (double)v, unit);
}

static void distance(const FwFlightSettings *s, double v, char *out, size_t cap){
    float f = (float)v;
    if (invalid(f)) { copy(out, cap, NA); return; }
    switch (s->units.dist) {
    case FW_DIST_MILES: small_distance(f * K_MILES, "mi", out, cap); break;
    case FW_DIST_NAUTICAL_MILES: small_distance(f / 1852.0f, "NM", out, cap); break;
    case FW_DIST_KILOMETERS: small_distance(f / 1000.0f, "km", out, cap); break;
    case FW_DIST_FEET: snprintf(out, cap, "%ldft", lround((double)f * K_FEET)); break;
    default: snprintf(out, cap, "%ldm", lroundf(f)); break;
    }
}

/* ------------------------------------------------------------ templates */

static void tracked_text(const FwFlight *f, const char *s, const char *missing, char *out, size_t cap){
    if (f->has_trk && *s) fw_flight_clean(s, out, cap);
    else copy(out, cap, missing);
}


/* ------------------------------------------------------------ owned: long names
 * Stock lets a line wider than its box run off the panel. The owned firmware
 * first shortens the obvious words (FLIGHT-SCREENS.md "Long text"), then
 * scrolls what still does not fit (fw_screens). Only when a line does not
 * fit; the owner's airport aliases are never shortened. */

static bool word_is(const char *p, size_t n, const char *w){
    size_t i;
    if (strlen(w) != n) return false;
    for (i = 0; i < n; ++i) {
        char a = p[i], b = w[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return false;
    }
    return true;
}

/* Rewrites s word by word through map ({from, to}; "" drops the word). */
static void map_words(char *s, size_t cap, const char *const (*map)[2]){
    char out[FW_FLIGHT_TEXT_MAX];
    size_t n = 0;
    const char *p = s;
    while (*p) {
        const char *e, *w;
        size_t len, wl, k;
        while (*p == ' ') ++p;
        if (!*p) break;
        for (e = p; *e && *e != ' '; ++e) {}
        len = (size_t)(e - p);
        w = p;
        wl = len;
        for (k = 0; map[k][0]; ++k)
            if (word_is(p, len, map[k][0])) { w = map[k][1]; wl = strlen(w); break; }
        if (wl && n + wl + 2 < sizeof out) {
            if (n) out[n++] = ' ';
            memcpy(out + n, w, wl);
            n += wl;
        }
        p = e;
    }
    out[n] = 0;
    if (n) copy(s, cap, out);
}

/* The last word, if it is one of words: removed (keeping at least one word). */
static bool drop_last(char *s, const char *const *words){
    char *sp = strrchr(s, ' ');
    if (!sp || sp == s) return false;
    for (; *words; ++words)
        if (word_is(sp + 1, strlen(sp + 1), *words)) { *sp = 0; return true; }
    return false;
}

/* "JetBlue Airways" -> "JetBlue", "Delta Air Lines" -> "Delta". A lone
 * "British" stays "British Airways". */
void fw_flight_short_airline(char *s, size_t cap){
    static const char *const suffix[] = {"Airways", "Airlines", "Airline", NULL};
    char keep[FW_FLIGHT_TEXT_MAX];
    size_t n = strlen(s);
    copy(keep, sizeof keep, s);
    if (n > 10 && word_is(s + n - 10, 10, " Air Lines")) s[n - 10] = 0;
    else if (!drop_last(s, suffix)) return;
    if (!*s || word_is(s, strlen(s), "British")) copy(s, cap, keep);
}

/* level 1: International -> Intl, Regional -> Rgnl, Municipal -> Muni ...,
 * "Airport" dropped; level 2 also drops a last Intl, Rgnl or Muni. */
void fw_flight_short_airport(char *s, size_t cap, int level){
    static const char *const map[][2] = {
        {"International", "Intl"}, {"Intl.", "Intl"}, {"Internacional", "Intl"}, {"Regional", "Rgnl"},
        {"Municipal", "Muni"}, {"Metropolitan", "Metro"}, {"Executive", "Exec"}, {"Memorial", "Mem"},
        {"County", "Co"}, {"Airport", ""}, {"Airfield", ""}, {"Aeropuerto", ""}, {"Aeroport", ""},
        {NULL, NULL}};
    static const char *const kind[] = {"Intl", "Rgnl", "Muni", NULL};
    if (level >= 1) map_words(s, cap, map);
    if (level >= 2) drop_last(s, kind);
}

static bool fits(const char *s, int w){ return w <= 0 || code_points(s) * 6 <= w; }

static void expand(const FwFlightSettings *s, const FwFlight *f, const char *tmpl, char *out, size_t cap, int level){
    char v[FW_FLIGHT_TEXT_MAX];
    if (!cap) return;
    if (!tmpl) { out[0] = 0; return; }
    copy(out, cap, tmpl);
#define SUB(tok) replace_all(out, cap, tok, v)
    /* owned airport aliases: the substituted text may be swapped first */
#define ALIAS(tok) do { char a_[FW_FLIGHT_TEXT_MAX]; \
        if (s && s->alias && s->alias(s->alias_ctx, tok, v, a_, sizeof a_)) copy(v, sizeof v, a_); } while (0)
    /* an airport name: the owner's alias as it is, else shortened at level */
#define ALIAS_SHORT(tok) do { char a_[FW_FLIGHT_TEXT_MAX]; \
        if (s && s->alias && s->alias(s->alias_ctx, tok, v, a_, sizeof a_)) copy(v, sizeof v, a_); \
        else if (level) fw_flight_short_airport(v, sizeof v, level); } while (0)
    title(s, f, v, sizeof v);
    if (level && s->title == FW_TITLE_AIRLINE && *f->al_name) fw_flight_short_airline(v, sizeof v);
    SUB("{al_name}");
    callsign(f, v, sizeof v);                        SUB("{cs}");
    registration(f, v, sizeof v);                    SUB("{reg}");
    cleaned_or_na(f->ac_name, v, sizeof v);          SUB("{ac_name}");
    origin(s, f, v, sizeof v);                       ALIAS_SHORT("{orig}"); SUB("{orig}");
    destination(s, f, v, sizeof v);                  ALIAS_SHORT("{dest}"); SUB("{dest}");
    copy(v, sizeof v, *f->orig_iata ? f->orig_iata : NA); ALIAS("{orig_iata}"); SUB("{orig_iata}");
    copy(v, sizeof v, *f->dest_iata ? f->dest_iata : NA); ALIAS("{dest_iata}"); SUB("{dest_iata}");
    altitude(s, f->alt_b, v, sizeof v);              SUB("{alt_b}");
    speed(s, f->vel, v, sizeof v);                   SUB("{vel}");
    heading(s, f->trk, v, sizeof v);                 SUB("{trk}");
    vertical(s, f->vr, v, sizeof v);                 SUB("{vr}");
    copy(v, sizeof v, *f->sqk ? f->sqk : NA);        SUB("{sqk}");
    if (f->has_trk && !nan_d(f->prog)) snprintf(v, 8, "%.0f%%", f->prog);
    else copy(v, sizeof v, NA);
    SUB("{trk_prog}");
    tracked_text(f, f->dep, NA, v, sizeof v);        SUB("{trk_dep}");
    tracked_text(f, f->arr, NA, v, sizeof v);        SUB("{trk_arr}");
    tracked_text(f, f->over, NA, v, sizeof v);       SUB("{trk_over}");
    tracked_text(f, f->conf, NA, v, sizeof v);       SUB("{trk_conf}");
    tracked_text(f, f->tft, "", v, sizeof v);        SUB("{trk_tft}");
    if (f->has_trk && !nan_d(f->dist)) distance(s, f->dist, v, sizeof v);
    else copy(v, sizeof v, NA);
    SUB("{trk_dist}");
#undef SUB
#undef ALIAS
#undef ALIAS_SHORT
}

void fw_flight_expand(const FwFlightSettings *s, const FwFlight *f, const char *tmpl, char *out, size_t cap){
    expand(s, f, tmpl, out, cap, 0);
}

/* As stock, then shorter names (level 1, then 2) while it does not fit w. */
static void expand_fit(const FwFlightSettings *s, const FwFlight *f, const char *tmpl, int w, char *out, size_t cap){
    int level;
    expand(s, f, tmpl, out, cap, 0);
    for (level = 1; level <= 2 && !fits(out, w); ++level) expand(s, f, tmpl, out, cap, level);
}

typedef struct { const char *element, *tmpl; } Tmpl;

static const Tmpl MINI_DEFAULT[] = {
    {"footer1", "{orig}"}, {"footer2", "{dest}"}, {"heading1", "{al_name}"},
    {"heading2", "{orig_iata}-{dest_iata}"}, {"heading3", "{ac_name}"}, {NULL, NULL},
};
static const Tmpl MINI_NO_LOGO[] = {
    {"footer1", "{dest}"}, {"footer2", "Alt:{alt_b},Spd:{vel}"}, {"heading1", "{al_name}"},
    {"heading2", "{orig_iata}-{dest_iata}, {ac_name}"}, {"heading3", "{orig}"}, {NULL, NULL},
};
static const Tmpl MINI_TECHNICAL[] = {
    {"footer1", "Alt:{alt_b},Spd:{vel}"}, {"footer2", "Trk:{trk},Vr:{vr}"}, {"heading1", "{al_name}"},
    {"heading2", "{orig_iata}-{dest_iata}"}, {"heading3", "{ac_name}"}, {NULL, NULL},
};
static const Tmpl MINI_SYNC[] = {
    {"heading1", "{cs}"}, {"heading2", "{orig_iata}-{dest_iata}"}, {"heading3", "{ac_name}"}, {NULL, NULL},
};
static const Tmpl WIDE[] = {   /* DEFAULT, LARGE_LOGO and NO_LOGO share it */
    {"footer1", "{orig}"}, {"footer2", "{dest}"}, {"heading1", "{al_name}"},
    {"heading2", "{orig_iata}-{dest_iata}"}, {"heading3", "{ac_name}"}, {"tag1", "Alt: {alt_b}"},
    {"tag2", "Spd: {vel}"}, {"tag3", "Trk: {trk}"}, {"tag4", "Vr: {vr}"}, {NULL, NULL},
};
static const Tmpl WIDE_SYNC[] = {
    {"heading1", "{cs}"}, {"heading2", "{orig_iata}-{dest_iata}"}, {"heading3", "{ac_name}"},
    {"tag1", "{alt_b}"}, {"tag2", "{vel}"}, {"tag3", "{trk}"}, {"tag4", "{vr}"}, {NULL, NULL},
};

const char *fw_flight_element_template(bool mini, const char *layout, const char *element){
    const Tmpl *t = NULL;
    if (mini) {
        if (eq(layout, "DEFAULT")) t = MINI_DEFAULT;
        else if (eq(layout, "NO_LOGO")) t = MINI_NO_LOGO;
        else if (eq(layout, "TECHNICAL")) t = MINI_TECHNICAL;
        else if (eq(layout, "PLUS_SYNC_ACTIVE_MAIN")) t = MINI_SYNC;
    } else {
        if (eq(layout, "DEFAULT") || eq(layout, "LARGE_LOGO") || eq(layout, "NO_LOGO")) t = WIDE;
        else if (eq(layout, "PLUS_SYNC_ACTIVE_MAIN")) t = WIDE_SYNC;
    }
    for (; t && t->element; ++t)
        if (eq(t->element, element)) return t->tmpl;
    return NULL;
}

/* FUN_420087bc: a registration too wide for heading1 gives way to the callsign. */
static void fit_heading1(const FwFlightSettings *s, const FwFlight *f, int element_w, char *out, size_t cap){
    if (s->title == FW_TITLE_REGISTRATION && *f->reg && strcmp(out, f->reg) == 0 &&
        element_w < code_points(out) * 6)
        callsign(f, out, cap);
}

static void shorten(char *out, size_t cap, size_t keep_bytes){
    char tmp[FW_FLIGHT_TEXT_MAX];
    snprintf(tmp, sizeof tmp, "%.*s...", (int)keep_bytes, out);
    copy(out, cap, tmp);
}

bool fw_flight_text(const FwFlightSettings *s, const FwFlight *f, const char *element, int element_w,
                    char *out, size_t cap){
    const char *tmpl = fw_flight_element_template(s->mini, s->layout, element);
    if (!tmpl || !cap) return false;
    expand_fit(s, f, tmpl, element_w, out, cap);
    if (eq(element, "heading1")) fit_heading1(s, f, element_w, out, cap);
    if (!s->mini && strlen(out) > 32 && (eq(element, "footer1") || eq(element, "heading1")))
        shorten(out, cap, 29);
    return true;
}

/* ------------------------------------------------------------ tracked */

void fw_track_state(const FwFlight *f, FwTrackState *t){
    bool known = f->has_trk && !nan_d(f->prog);
    t->landed = known && f->prog >= 100.0;
    t->on_ground = known && f->prog == 0.0 && f->trk_gnd;
    t->took = t->landed && f->has_trk && *f->tft;
}

int fw_track_pages(const FwFlight *f, bool show_metrics, uint8_t pages[FW_TRACK_MAX_PAGES]){
    int n = 0;
    pages[n++] = 0;
    pages[n++] = 1;   /* "Flying over": always, outside PLUS sync mode */
    if (show_metrics && (!nan_d(f->vel) || !nan_d(f->trk) || !nan_d(f->vr) || !nan_d(f->alt_b)))
        pages[n++] = 2;
    if (f->has_trk && !nan_d(f->dist)) pages[n++] = 3;
    return n;
}

uint32_t fw_track_page_ms(int page_count, int tracked_flights){
    uint32_t ms = 5000;
    if (tracked_flights > 1 && page_count > 2) {
        ms = (ms * 2) / (uint32_t)page_count;
        if (ms < 1000) ms = 1000;
    }
    return ms;
}

static const char *track_footer(const FwTrackState *t, int page, bool first){
    if (t->on_ground) return first ? "On ground" : "Preparing to takeoff";
    if (t->landed) return first ? (t->took ? "Took {trk_tft}" : NULL) : "Flight landed";
    switch (page) {
    case 0: return first ? "Departed {trk_dep}" : "Arriving in {trk_arr}";
    case 1: return first ? "Flying over" : "{trk_over}";
    case 2: return first ? "Alt:{alt_b},Spd:{vel}" : "Trk:{trk},Vr:{vr}";
    case 3: return first ? "{trk_dist}" : "Distance flown";
    default: return NULL;
    }
}

const char *fw_track_template(const FwFlightSettings *s, const FwTrackState *t, int page, const char *element){
    if (eq(element, "heading1")) return s->title == FW_TITLE_REGISTRATION ? "{reg}" : "{cs}";
    if (eq(element, "heading2")) return t->on_ground ? "{ac_name}" : "{orig_iata}-{dest_iata}";
    if (eq(element, "heading3")) return t->on_ground ? (s->mini ? "{vel}" : NULL) : "{ac_name}";
    if (eq(element, "footer1")) return track_footer(t, page, true);
    if (eq(element, "footer2")) return track_footer(t, page, false);
    return fw_flight_element_template(s->mini, s->layout, element);
}

bool fw_track_text(const FwFlightSettings *s, const FwFlight *f, const FwTrackState *t, int page,
                   const char *element, int element_w, int64_t now, char *out, size_t cap){
    const char *tmpl = fw_track_template(s, t, page, element);
    bool footer = eq(element, "footer1") || eq(element, "footer2");
    if (!tmpl || !cap) return false;
    if (eq(tmpl, "{ac_name}") && f->has_trk && now > 0 && now < f->sexp && f->sexp - now <= 120 &&
        (f->sts == 'C' || f->sts == 'D'))
        copy(out, cap, f->sts == 'C' ? "CANCELLED" : "DELAYED");
    else if (footer)
        fw_flight_expand(s, f, tmpl, out, cap);
    else
        expand_fit(s, f, tmpl, element_w, out, cap);
    if (eq(element, "heading1")) fit_heading1(s, f, element_w, out, cap);
    if (strcmp(out, "Arriving in now") == 0) copy(out, cap, "Arriving now");
    if (footer && s->mini && code_points(out) > 21) shorten(out, cap, prefix_bytes(out, 18));
    return true;
}
