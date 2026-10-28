/* The dashboard's display preview (DASHBOARD.md "Preview"): the wall's own
 * screen code, built for the browser (tools/build_preview.py ->
 * main/preview.wasm), drawing the settings being edited before they are saved.
 *
 * It runs the live screens the way main/display.cpp's live_step does: the
 * flights in rotation, tracked flights page by page, or the loading template
 * when there are none, with the settings parsed by the same fw_feed_display().
 * What the page supplies instead of the board: the time, the settings JSON,
 * the flights (the wall's own, GET /api/flights, or samples), the airport
 * aliases and the logo files (GET /api/logo). Brightness and sleep are not in
 * the pixels on the board either; the page dims the picture itself.
 *
 * The frame is 128 x 64 RGBA (alpha 255), ready for an ImageData. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "fw_screens.h"
#include "fw_clock.h"
#include "fw_feed.h"
#include "fw_logo.h"
#include "owner_alias.h"

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#else
#define EXPORT(name)
#endif

#define W 128
#define H 64

static uint8_t s_fb[W * H * 4];
static int s_changed;

static void put(void *ctx, int x, int y, FwRgb c){
    uint8_t *p = &s_fb[(y * W + x) * 4];
    (void)ctx;
    if (p[0] == c.r && p[1] == c.g && p[2] == c.b) return;
    p[0] = c.r; p[1] = c.g; p[2] = c.b;
    s_changed = 1;
}

static const FwSurface S = {NULL, W, H, put, NULL};

/* ------------------------------------------------------------ memory for the page */

EXPORT(pv_alloc) void *pv_alloc(size_t n){ return malloc(n ? n : 1); }
EXPORT(pv_free) void pv_free(void *p){ free(p); }
EXPORT(pv_frame) const uint8_t *pv_frame(void){ return s_fb; }

/* ------------------------------------------------------------ logos */

/* Decoded logos by airline code and aircraft class, as the wall resolves them
 * (the page fetches /api/logo?al=..&ac=.. for each one asked for). */
#define LOGOS 8
typedef struct {
    char al[8], ac[24];
    int state;                 /* 0 asked for, 1 decoded, 2 none */
    uint32_t used;
    FwLogo logo;
} LogoSlot;
static LogoSlot s_logos[LOGOS];
static uint32_t s_logo_clock;
static const FwLogo *s_shown_logo;
static char s_shown_al[8], s_shown_ac[24];
static int s_logo_arrived;

static LogoSlot *logo_slot(const char *al, const char *ac, int add){
    LogoSlot *oldest = &s_logos[0];
    for (int i = 0; i < LOGOS; ++i) {
        LogoSlot *l = &s_logos[i];
        if (l->al[0] && !strcmp(l->al, al) && !strcmp(l->ac, ac)) return l;
        if (l->used < oldest->used) oldest = l;
    }
    if (!add) return NULL;
    memset(oldest, 0, sizeof *oldest - sizeof oldest->logo);
    snprintf(oldest->al, sizeof oldest->al, "%s", al);
    snprintf(oldest->ac, sizeof oldest->ac, "%s", ac);
    oldest->used = ++s_logo_clock;
    return oldest;
}

/* display.cpp load_logo: the logo for the flight about to be drawn. */
static void load_logo(const char *al, const char *ac){
    LogoSlot *l = logo_slot(al, ac ? ac : "", 1);
    l->used = ++s_logo_clock;
    snprintf(s_shown_al, sizeof s_shown_al, "%s", al);
    snprintf(s_shown_ac, sizeof s_shown_ac, "%s", ac ? ac : "");
    s_shown_logo = l->state == 1 ? &l->logo : NULL;
}

/* The i-th logo the screens asked for and the page has not supplied yet, as
 * "AL|class", or NULL. */
EXPORT(pv_logo_wanted) const char *pv_logo_wanted(int i){
    static char key[40];
    for (int k = 0; k < LOGOS; ++k) {
        if (!s_logos[k].al[0] || s_logos[k].state) continue;
        if (i-- == 0) {
            snprintf(key, sizeof key, "%s|%s", s_logos[k].al, s_logos[k].ac);
            return key;
        }
    }
    return NULL;
}

/* The logo file for al/ac (len 0: the wall has none). */
EXPORT(pv_logo) void pv_logo(const char *al, const char *ac, const uint8_t *png, size_t len){
    LogoSlot *l = logo_slot(al, ac, 1);
    void *dec;
    const FwElement *box = &fw_layout_mini_default[0];
    l->state = 2;
    if (len && (dec = malloc(fw_logo_decoder_bytes()))) {
        if (fw_logo_decode(dec, png, len, fw_logo_side_for_box(box->w, box->h), &l->logo) == FW_LOGO_OK) l->state = 1;
        free(dec);
    }
    if (!strcmp(al, s_shown_al) && !strcmp(ac, s_shown_ac)) {   /* the card on show: draw it again with it */
        s_shown_logo = l->state == 1 ? &l->logo : NULL;
        s_logo_arrived = 1;
    }
}

/* ------------------------------------------------------------ aliases */

static OwnerAliasContext s_alias;
static int s_alias_ready;

static int alias_absent(void *u, const char *ns, const char *key, void *buf, size_t *len){
    (void)u; (void)ns; (void)key; (void)buf; (void)len;
    return OA_NOT_FOUND;
}

static int alias_no_write(void *u, const char *ns, const char *key, const void *buf, size_t len){
    (void)u; (void)ns; (void)key; (void)buf; (void)len;
    return OA_STORAGE;                   /* the preview never saves */
}

/* main/aliases.c fw_aliases_lookup */
static bool alias_lookup(void *ctx, const char *token, const char *value, char *out, size_t cap){
    char found[OA_TEXT_CAP];
    (void)ctx;
    if (!s_alias_ready || !value || strlen(value) >= OA_TEXT_CAP) return false;
    if (owner_alias_lookup(&s_alias, token, value, found) != OA_OK) return false;
    snprintf(out, cap, "%s", found);
    return true;
}

static int s_enter = 1;

/* The aliases being edited: [{"kind":"iata","from":"DJT","to":"PBI"},...].
 * An invalid list shows none (the dashboard checks them before saving). */
EXPORT(pv_aliases) void pv_aliases(const char *json){
    static const OwnerAliasOps ops = {NULL, alias_absent, alias_no_write, NULL, NULL};
    static OwnerAliasEntry seed[OA_MAX_ALIASES];
    cJSON *root = cJSON_Parse(json), *a;
    uint32_t n = 0;
    s_alias_ready = 0;
    cJSON_ArrayForEach(a, cJSON_IsArray(root) ? root : NULL) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "kind"));
        const char *from = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "from"));
        const char *to = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "to"));
        if (n >= OA_MAX_ALIASES || !kind || !from || !to) { n = 0; break; }
        memset(&seed[n], 0, sizeof seed[n]);
        seed[n].kind = !strcmp(kind, "name") ? OA_NAME : !strcmp(kind, "iata") ? OA_IATA : !strcmp(kind, "icao") ? OA_ICAO : 0;
        snprintf(seed[n].from, sizeof seed[n].from, "%s", from);
        snprintf(seed[n].to, sizeof seed[n].to, "%s", to);
        ++n;
    }
    cJSON_Delete(root);
    if (n && owner_alias_init(&s_alias, &ops, seed, n) == OA_OK) s_alias_ready = 1;
    s_enter = 1;
}

/* ------------------------------------------------------------ settings (display.cpp live_settings) */

static FwFeedDisplay s_disp;
static FwFlightSettings s_st;
static const char *s_anchors[8];
static FwFrame s_frame;
static FwRotation s_rot;
static FwClockConfig s_clock;
static FwClockFrame s_clock_prev;

static uint8_t dst_rule(const char *r){
    return !strcmp(r, "north_america") ? FW_DST_NORTH_AMERICA : !strcmp(r, "europe") ? FW_DST_EUROPE : FW_DST_NONE;
}

/* The whole settings document being edited (display_config, owned_config...). */
EXPORT(pv_settings) void pv_settings(const char *json){
    const FwFeedDisplay *d = &s_disp;
    fw_feed_display(json, &s_disp);
    memset(&s_st, 0, sizeof s_st);
    s_st.units = d->units;
    s_st.title = d->title;
    s_st.mini = true;
    s_st.layout = d->layout;
    for (int i = 0; i < d->anchor_count; ++i) s_anchors[i] = d->anchors[i];
    s_st.anchors = s_anchors;
    s_st.anchor_count = d->anchor_count;
    s_st.alias = alias_lookup;
    s_st.progress = d->progress_bar;
    s_frame.border = d->border;
    s_frame.border_percent = d->border_percent;
    s_frame.indicator = false;
    fw_rotation_init(&s_rot, d->max_flights, d->dwell_ms);
    fw_screen_scroll_speed(d->scroll_px_ms ? d->scroll_px_ms : FW_SCROLL_PX_MS);
    memset(&s_clock, 0, sizeof s_clock);
    s_clock.zone.standard_offset_min = d->utc_offset_min;
    s_clock.zone.dst_rule = dst_rule(d->dst_rule);
    s_clock.large = d->clock_large;
    s_clock.show_date = d->clock_show_date;
    s_clock.is_24h = d->clock_24h;
    s_clock.show_seconds = d->clock_seconds;
    s_clock.day_first = d->clock_day_first;
    s_clock.show_weekday = d->clock_weekday;
    s_clock.locale = d->locale;
    s_clock.extra_count = d->extra_count < FW_CLOCK_MAX_EXTRA_ZONES ? d->extra_count : FW_CLOCK_MAX_EXTRA_ZONES;
    for (int i = 0; i < s_clock.extra_count; ++i) {
        FwExtraZone *x = &s_clock.extra[i];
        snprintf(x->label, sizeof x->label, "%s", d->extra[i].label);
        x->zone.standard_offset_min = d->extra[i].utc_offset_min;
        x->zone.dst_rule = dst_rule(d->extra[i].dst_rule);
    }
    s_enter = 1;
}

/* Brightness and power, which the board applies to the panel, not the pixels:
 * 0 display_enabled, 1 brightness_percent (0 = not given), 2 has_sleep,
 * 3 sleep start, 4 sleep end (minutes after local midnight), 5 sleep percent,
 * 6 local minutes now for a unix time given in pv_step. */
static double s_unix;
EXPORT(pv_power) int pv_power(int what){
    FwTime t;
    switch (what) {
    case 0: return s_disp.display_enabled;
    case 1: return s_disp.brightness_percent;
    case 2: return s_disp.has_sleep;
    case 3: return s_disp.sleep_start_min;
    case 4: return s_disp.sleep_end_min;
    case 5: return s_disp.sleep_percent;
    case 6: return fw_local_time((int64_t)s_unix, s_clock.zone, &t) ? t.hour * 60 + t.min : -1;
    }
    return 0;
}

/* ------------------------------------------------------------ flights */

static FwFeedFrame s_live;
static int s_fresh;

static void str(char *out, size_t cap, const cJSON *o, const char *key){
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    snprintf(out, cap, "%s", v ? v : "");
}

static double num(const cJSON *o, const char *key){
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static bool flag(const cJSON *o, const char *key){ return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key)); }

/* {"flights":[{FwFlight's fields by name}...],"tracked":false,"overflow":0},
 * as GET /api/flights gives them (dashboard_api.c). */
EXPORT(pv_flights) void pv_flights(const char *json){
    cJSON *root = cJSON_Parse(json), *a;
    memset(&s_live, 0, sizeof s_live);
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(root, "flights")) {
        FwFlight *f = &s_live.flights[s_live.count];
        const char *sts;
        if (s_live.count >= FW_FEED_MAX_FLIGHTS) break;
        fw_flight_init(f);
        str(f->icao, sizeof f->icao, a, "icao"); str(f->cs, sizeof f->cs, a, "cs"); str(f->reg, sizeof f->reg, a, "reg");
        f->alt_b = num(a, "alt_b"); f->alt_g = num(a, "alt_g"); f->vel = num(a, "vel");
        f->trk = num(a, "trk"); f->vr = num(a, "vr"); f->gnd = flag(a, "gnd");
        str(f->sqk, sizeof f->sqk, a, "sqk");
        str(f->orig_icao, sizeof f->orig_icao, a, "orig_icao"); str(f->orig_iata, sizeof f->orig_iata, a, "orig_iata");
        str(f->dest_icao, sizeof f->dest_icao, a, "dest_icao"); str(f->dest_iata, sizeof f->dest_iata, a, "dest_iata");
        str(f->orig, sizeof f->orig, a, "orig"); str(f->dest, sizeof f->dest, a, "dest");
        str(f->al_code, sizeof f->al_code, a, "al_code"); str(f->al_paint, sizeof f->al_paint, a, "al_paint");
        str(f->al_op, sizeof f->al_op, a, "al_op"); str(f->al_name, sizeof f->al_name, a, "al_name");
        str(f->ac_type, sizeof f->ac_type, a, "ac_type"); str(f->ac_name, sizeof f->ac_name, a, "ac_name");
        str(f->ac_class, sizeof f->ac_class, a, "ac_class");
        f->has_trk = flag(a, "has_trk");
        sts = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "sts"));
        f->sts = sts && (*sts == 'C' || *sts == 'D') ? *sts : 0;
        f->sexp = (int64_t)(isnan(num(a, "sexp")) ? 0 : num(a, "sexp"));
        f->prog = num(a, "prog"); f->dist = num(a, "dist");
        str(f->conf, sizeof f->conf, a, "conf"); str(f->dep, sizeof f->dep, a, "dep"); str(f->arr, sizeof f->arr, a, "arr");
        str(f->over, sizeof f->over, a, "over"); str(f->tft, sizeof f->tft, a, "tft");
        f->trk_gnd = flag(a, "trk_gnd"); f->trk_metrics = flag(a, "trk_metrics");
        ++s_live.count;
    }
    s_live.tracked = flag(root, "tracked");
    s_live.overflow = (int)(isnan(num(root, "overflow")) ? 0 : num(root, "overflow"));
    cJSON_Delete(root);
    s_fresh = 1;                         /* like a new feed response: the rotation carries on */
}

/* ------------------------------------------------------------ the screens (display.cpp live_step) */

enum { PV_LIVE, PV_EMPTY, PV_CLOCK };
enum LiveMode { LiveNone, LiveRotation, LiveTracked, LiveEmpty, LiveClock };
static int s_want = PV_LIVE, s_mode = LiveNone;
static uint32_t s_now, s_card_at;
static int s_trk_flight, s_trk_page_i, s_trk_pages_n;
static uint8_t s_trk_pages[FW_TRACK_MAX_PAGES];
static uint32_t s_trk_since;

/* 0 the flights (the loading template when there are none), 1 the loading
 * template, 2 the clock. */
EXPORT(pv_mode) void pv_mode(int m){ s_want = m; s_enter = 1; }

static void card_drawn(void){ s_card_at = s_now; }
static void card_hold(FwRotation *r){
    uint32_t pass = fw_screen_scroll_pass_ms();
    r->extra_ms = pass > r->dwell_ms ? pass - r->dwell_ms : 0;
}
static void card_scroll(void){ if (fw_screen_scroll_count()) fw_screen_scroll_tick(&S, s_now - s_card_at); }

static void clock_refresh(void){
    FwClockFrame f;
    fw_clock_frame(&s_clock, (int64_t)s_unix, W, H, &f);
    fw_clock_draw(&S, &f, &s_clock_prev);
}

static void clock_enter(void){
    memset(&s_clock_prev, 0, sizeof s_clock_prev);
    clock_refresh();
}

static int clock_set(void){ return s_unix > 1700000000; }

static void live_empty(bool enter){
    const char *t = s_disp.loading;
    if (!strcmp(t, "CLOCK") && clock_set()) {
        if (enter) clock_enter(); else clock_refresh();
    } else if (enter) {
        if (!strcmp(t, "BLANK")) fw_clear(&S);
        else fw_screen_message(&S, &s_frame, !strcmp(t, "ELLIPSIS") ? "..." : "No flights...");
    }
}

static void tracked_draw(void){
    const FwFlight *f = &s_live.flights[s_trk_flight];
    if (strcmp(s_shown_al, f->al_code) || strcmp(s_shown_ac, f->ac_class)) load_logo(f->al_code, f->ac_class);
    fw_screen_tracked(&S, &s_st, f, s_trk_pages[s_trk_page_i], (int64_t)s_unix, s_shown_logo);
    fw_screen_frame(&S, &s_frame);
    card_drawn();
}

static void tracked_start(int flight){
    const FwFlight *f = &s_live.flights[flight];
    s_trk_flight = flight;
    s_trk_page_i = 0;
    s_trk_pages_n = fw_track_pages(f, f->trk_metrics, s_trk_pages);
    if (!f->over[0]) {                       /* nothing to say under "Flying over": skip it */
        int n = 0;
        for (int i = 0; i < s_trk_pages_n; ++i) if (s_trk_pages[i] != 1) s_trk_pages[n++] = s_trk_pages[i];
        s_trk_pages_n = n;
    }
    if (s_trk_pages_n < 1) { s_trk_pages[0] = 0; s_trk_pages_n = 1; }
    s_trk_since = s_now;
}

static void tracked_tick(void){
    uint32_t page = fw_track_page_ms(s_trk_pages_n, s_live.count), pass = fw_screen_scroll_pass_ms();
    if (s_logo_arrived) { s_logo_arrived = 0; tracked_draw(); return; }
    if (s_now - s_trk_since < (pass > page ? pass : page)) { card_scroll(); return; }
    if (++s_trk_page_i >= s_trk_pages_n) tracked_start((s_trk_flight + 1) % s_live.count);
    else s_trk_since = s_now;
    tracked_draw();
}

static void rotation_tick(bool redraw_anyway){
    const FwFlight *f;
    int more;
    bool redraw;
    FwShow show = fw_rotation_tick(&s_rot, s_now, &f, &more, &redraw);
    if (s_logo_arrived) { s_logo_arrived = 0; redraw_anyway = true; }
    if (!redraw && !redraw_anyway) { card_scroll(); return; }
    s_frame.indicator = s_rot.indicator;
    if (show == FW_SHOW_FLIGHT && !f && redraw_anyway) {   /* keep the card, draw it again (a logo came) */
        for (int i = 0; i < s_live.count; ++i)
            if (!strcmp(s_live.flights[i].icao, s_rot.cur_icao) && !strcmp(s_live.flights[i].cs, s_rot.cur_cs))
                f = &s_live.flights[i];
    }
    if (show == FW_SHOW_FLIGHT && f) {
        if (strcmp(s_shown_al, f->al_code) || strcmp(s_shown_ac, f->ac_class)) load_logo(f->al_code, f->ac_class);
        fw_screen_flight(&S, &s_st, f, s_shown_logo);
        fw_screen_frame(&S, &s_frame);
        card_drawn();
        card_hold(&s_rot);
    } else if (show == FW_SHOW_TOO_MANY) {
        fw_screen_too_many(&S, &s_frame, more);
    }
}

/* Advances the preview to now_ms (any steady milliseconds) at unix time
 * unix_s; returns 1 when the frame changed. */
EXPORT(pv_step) int pv_step(uint32_t now_ms, double unix_s){
    int mode;
    bool enter = s_enter, fresh = s_fresh;
    s_now = now_ms;
    s_unix = unix_s;
    s_changed = 0;
    s_enter = 0;
    s_fresh = 0;
    if (enter) {
        fw_screen_scroll_reset();
        s_shown_al[0] = 0;              /* draw the logo afresh */
    }
    mode = s_want == PV_CLOCK ? LiveClock : s_want == PV_EMPTY || !s_live.count ? LiveEmpty
         : s_live.tracked ? LiveTracked : LiveRotation;
    if (mode != s_mode || enter) {
        s_mode = mode;
        if (mode == LiveRotation) {
            fw_rotation_init(&s_rot, s_disp.max_flights, s_disp.dwell_ms);
            fw_rotation_set_list(&s_rot, s_live.flights, s_live.count, s_live.overflow);
            rotation_tick(true);
        } else if (mode == LiveTracked) {
            tracked_start(0);
            tracked_draw();
        } else if (mode == LiveClock) {
            clock_enter();
        } else {
            live_empty(true);
        }
        return s_changed;
    }
    if (mode == LiveRotation) {
        if (fresh) fw_rotation_set_list(&s_rot, s_live.flights, s_live.count, s_live.overflow);
        rotation_tick(false);
    } else if (mode == LiveTracked) {
        if (fresh) {
            if (s_trk_flight >= s_live.count) tracked_start(0);
            tracked_draw();                  /* same page, new numbers */
        }
        tracked_tick();
    }
    else if (mode == LiveClock) clock_refresh();
    else live_empty(false);
    return s_changed;
}
