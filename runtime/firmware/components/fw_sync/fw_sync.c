/* Stock "Plus Sync" rules and screens; see fw_sync.h and SYNC.md. */
#include "fw_sync.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const FwRgb WHITE = {255, 255, 255};
static const FwRgb CYAN = {0, 255, 255};       /* 0x07FF */
static const FwRgb GREEN = {0, 255, 0};        /* 0x07E0 */
static const FwRgb GREY = {132, 130, 132};     /* 0x8410 */

static void copy(char *out, size_t cap, const char *s){
    if (cap) snprintf(out, cap, "%s", s ? s : "");
}

void fw_sync_leg_init(FwSyncLeg *leg){
    memset(leg, 0, sizeof *leg);
}

void fw_sync_flight(const FwSyncLeg *leg, char *out, size_t cap){
    if (leg->al[0] && leg->fn[0]) snprintf(out, cap, "%s%s", leg->al, leg->fn);
    else copy(out, cap, "");
}

void fw_sync_route(const FwSyncLeg *leg, char *out, size_t cap){
    snprintf(out, cap, "%s-%s", leg->orig[0] ? leg->orig : "---", leg->dest[0] ? leg->dest : "---");
}

bool fw_sync_canceled(const FwSyncLeg *leg){ return strcmp(leg->ls, "canceled") == 0; }

const char *fw_sync_status(const FwSyncLeg *leg){
    if (fw_sync_canceled(leg)) return "Canceled";
    if (strcmp(leg->lt, "operating") == 0) return "Operating";
    if (strcmp(leg->lt, "deadhead") == 0) return "Deadhead";
    return "";
}

const char *fw_sync_strip_in(const char *rel){
    return strncmp(rel, "in ", 3) == 0 ? rel + 3 : rel;
}

/* ------------------------------------------------------------ time */

static bool digits(const char **p, int n, int *out){
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if ((*p)[i] < '0' || (*p)[i] > '9') return false;
        v = v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    *out = v;
    return true;
}

/* days since 1970-01-01 of a civil date */
static int64_t days_from_civil(int y, int m, int d){
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t fw_sync_parse_time(const char *p){
    int y, mo, d, h, mi, sec = 0, oh = 0, om = 0, sign = 0;
    int64_t t;
    if (!p) return 0;
    if (!digits(&p, 4, &y) || *p++ != '-' || !digits(&p, 2, &mo) || *p++ != '-' || !digits(&p, 2, &d)) return 0;
    if (*p != 'T' && *p != 't' && *p != ' ') return 0;
    ++p;
    if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi)) return 0;
    if (*p == ':') {
        ++p;
        if (!digits(&p, 2, &sec)) return 0;
    }
    if (*p == '.' || *p == ',') {
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == 'Z' || *p == 'z') {
        ++p;
    } else if (*p == '+' || *p == '-') {
        sign = *p++ == '-' ? -1 : 1;
        if (!digits(&p, 2, &oh)) return 0;
        if (*p == ':') ++p;
        if (*p >= '0' && *p <= '9' && !digits(&p, 2, &om)) return 0;
    }
    if (*p) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60 || oh > 23 || om > 59) return 0;
    if (sec == 60) sec = 59;
    t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - sign * (oh * 3600 + om * 60);
    return t > 0 && t <= 0xffffffffLL ? (uint32_t)t : 0;
}

/* ------------------------------------------------------------ text */

static int cp_count(const char *s){
    int n = 0;
    for (; *s; ++s) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

/* byte length of the first n code points */
static size_t cp_bytes(const char *s, int n){
    size_t i = 0;
    while (s[i] && n > 0) {
        ++i;
        while (((unsigned char)s[i] & 0xc0) == 0x80) ++i;
        --n;
    }
    return i;
}

static bool is_space(char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

void fw_sync_fit(const char *s, int n, char *out, size_t cap){
    size_t len, start = 0;
    if (!cap) return;
    out[0] = 0;
    if (!s || n <= 0) return;
    if (cp_count(s) <= n) { copy(out, cap, s); return; }
    len = cp_bytes(s, n - 1);
    while (start < len && is_space(s[start])) ++start;          /* Arduino String trim() */
    while (len > start && is_space(s[len - 1])) --len;
    snprintf(out, cap, "%.*s.", (int)(len - start), s + start);
}

void fw_sync_wrap(const char *text, int max, char *l1, char *l2, size_t cap){
    char *line[2] = {l1, l2};
    int cur = 0;
    size_t used[2] = {0, 0};
    l1[0] = l2[0] = 0;
    while (text && *text) {
        const char *w;
        size_t wl;
        while (*text == ' ') ++text;
        if (!*text) break;
        w = text;
        while (*text && *text != ' ') ++text;
        wl = (size_t)(text - w);
        if (cur == 0 && used[0] &&
            cp_count(line[0]) + 1 + (int)(cp_count(w) - cp_count(text)) > max)
            cur = 1;
        if (used[cur] + (used[cur] ? 1 : 0) + wl + 1 > cap) break;
        if (used[cur]) line[cur][used[cur]++] = ' ';
        memcpy(line[cur] + used[cur], w, wl);
        used[cur] += wl;
        line[cur][used[cur]] = 0;
    }
}

void fw_sync_ident(const FwSyncLeg *leg, int phase, char *out, size_t cap){
    char flight[16];
    const char *status = fw_sync_status(leg);
    fw_sync_flight(leg, flight, sizeof flight);
    if (flight[0] && status[0]) copy(out, cap, (phase & 1) ? status : flight);
    else copy(out, cap, flight[0] ? flight : status);
}

static bool ends_with(const char *s, const char *tail){
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && strcmp(s + n - t, tail) == 0;
}

static void arriving(char *out, size_t cap, const char *when){
    if (strcmp(when, "now") == 0) copy(out, cap, "Arriving now");
    else snprintf(out, cap, "Arriving in %s", when);
}

void fw_sync_footers(const FwSyncLeg *leg, const FwSyncLive *live, char *f1, char *f2, size_t cap){
    copy(f1, cap, "");
    copy(f2, cap, "");
    if (live && live->trk_dep && live->trk_dep[0]) {
        snprintf(f1, cap, "Departed %s", live->trk_dep);
        if (live->trk_arr && live->trk_arr[0]) arriving(f2, cap, live->trk_arr);
    } else if (fw_sync_canceled(leg)) {
        copy(f1, cap, "Flight canceled");
        if (leg->dep_rel[0]) snprintf(f2, cap, "Scheduled %s", leg->dep_rel);
    } else if (!leg->dep_rel[0]) {
        /* nothing known */
    } else if (ends_with(leg->dep_rel, "ago")) {
        snprintf(f1, cap, "Departed %s", leg->dep_rel);
        if (leg->arr_rel[0]) arriving(f2, cap, fw_sync_strip_in(leg->arr_rel));
    } else {
        snprintf(f1, cap, "Departs %s", leg->dep_rel);
        if (leg->arr_rel[0]) snprintf(f2, cap, "Arrives %s", leg->arr_rel);
    }
}

void fw_sync_message_footers(const char *msg, int max, char *f1, char *f2, size_t cap){
    char a[FW_SYNC_MESSAGE_MAX], b[FW_SYNC_MESSAGE_MAX];
    fw_sync_wrap(msg, max, a, b, sizeof a);
    fw_sync_fit(a, max, f1, cap);
    fw_sync_fit(b, max, f2, cap);
}

int fw_sync_context(const FwSyncLeg *leg, const FwSyncLive *live, char lines[FW_SYNC_CONTEXT_MAX][40]){
    int n = 0;
    fw_sync_route(leg, lines[n++], 40);
    if (!fw_sync_canceled(leg)) {
        const char *arr = live && live->trk_arr ? live->trk_arr : fw_sync_strip_in(leg->arr_rel);
        if (arr[0]) arriving(lines[n++], 40, arr);
    }
    if (live && live->trk_over && live->trk_over[0]) snprintf(lines[n++], 40, "Over %s", live->trk_over);
    if (live && live->dist && live->dist[0]) snprintf(lines[n++], 40, "%s flown", live->dist);
    return n;
}

double fw_sync_progress(const FwSyncLeg *leg, const FwSyncLive *live, int64_t now){
    if (live && live->prog == live->prog) return live->prog;
    if (fw_sync_canceled(leg) || !leg->dep_at || !leg->arr_at || leg->dep_at >= leg->arr_at) return NAN;
    if (now <= (int64_t)leg->dep_at) return 0.0;
    if (now >= (int64_t)leg->arr_at) return 100.0;
    return (double)(now - (int64_t)leg->dep_at) / (double)(leg->arr_at - leg->dep_at) * 100.0;
}

uint32_t fw_sync_phase_ms(FwSyncPhaseKind kind, int count, bool has_message, uint32_t slot_ms){
    int mult;
    switch (kind) {
    case FW_SYNC_PHASE_ACTIVE: mult = has_message ? 2 : 1; break;
    default: mult = count < 1 ? 1 : count; break;
    }
    return (uint32_t)mult * slot_ms;
}

/* ------------------------------------------------------------ screens */

typedef struct { int16_t x, y, w, h; } Box;

/* mini-v1 geometry from the firmware tables (msg 0x3c0f203c, next 0x3c0f2760,
 * list 0x3c0f1f78) */
static const Box MSG1 = {2, 2, 124, 18}, MSG2 = {2, 20, 124, 18}, IDENT = {2, 39, 124, 10}, CONTEXT = {2, 52, 124, 10};

static void begin(const FwSurface *s, const FwFrame *frame){
    fw_clear(s);
    fw_screen_frame(s, frame);
}

static int text_w(const char *t, int scale){ return fw_text_glyph_count(t) * FW_FONT_ADVANCE * scale; }

/* centred in the box: x + (w - tw)/2 when the slack is more than 1 */
static int centre_x(Box b, const char *t, int scale){
    int slack = b.w - text_w(t, scale);
    return slack > 1 ? b.x + slack / 2 : b.x;
}

static void line_in_box(const FwSurface *s, Box b, const char *t, int scale){
    int slack_y = b.h - FW_FONT_LINE * scale;
    if (!t[0]) return;
    fw_text_draw_bg(s, centre_x(b, t, scale), b.y + (slack_y > 1 ? slack_y / 2 : 0), t, WHITE, (FwRgb){0, 0, 0}, scale);
}

void fw_screen_sync_message(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *leg,
                            const FwSyncLive *live, const char *msg, int phase, int64_t now){
    char l1[FW_SYNC_MESSAGE_MAX], l2[FW_SYNC_MESSAGE_MAX], text[48];
    char ctx[FW_SYNC_CONTEXT_MAX][40];
    int max2 = MSG1.w / (FW_FONT_ADVANCE * 2), max1 = MSG1.w / FW_FONT_ADVANCE, n;
    double prog;
    begin(s, frame);

    if (msg && msg[0]) {
        fw_sync_wrap(msg, max2, l1, l2, sizeof l1);
        if (cp_count(l1) <= max2 && cp_count(l2) <= max2) {
            line_in_box(s, MSG1, l1, 2);
            line_in_box(s, MSG2, l2, 2);
        } else {
            const char *lines[2];
            int count = 0, span = MSG2.y + MSG2.h - MSG1.y, y;
            fw_sync_wrap(msg, max1, l1, l2, sizeof l1);
            if (!(cp_count(l1) <= max1 && cp_count(l2) <= max1)) {
                char a[FW_SYNC_MESSAGE_MAX];
                fw_sync_fit(l1, max1, a, sizeof a); snprintf(l1, sizeof l1, "%s", a);
                fw_sync_fit(l2, max1, a, sizeof a); snprintf(l2, sizeof l2, "%s", a);
            }
            if (l1[0]) lines[count++] = l1;
            if (l2[0]) lines[count++] = l2;
            y = MSG1.y + (span - (count * FW_FONT_LINE + (count - 1) * 2)) / 2;
            for (int i = 0; i < count; ++i, y += FW_FONT_LINE + 2)
                fw_text_draw_bg(s, centre_x(MSG1, lines[i], 1), y, lines[i], WHITE, (FwRgb){0, 0, 0}, 1);
        }
    }

    fw_sync_ident(leg, phase, l1, sizeof l1);
    fw_sync_fit(l1, IDENT.w / FW_FONT_ADVANCE, text, sizeof text);
    line_in_box(s, IDENT, text, 1);

    n = fw_sync_context(leg, live, ctx);
    fw_sync_fit(ctx[(phase < 0 ? 0 : phase) % n], CONTEXT.w / FW_FONT_ADVANCE, text, sizeof text);
    line_in_box(s, CONTEXT, text, 1);

    prog = fw_sync_progress(leg, live, now);
    if (prog == prog) fw_screen_progress(s, prog);
}

static bool digit_then_d(const char *s){
    for (; s[0] && s[1]; ++s)
        if (s[0] >= '0' && s[0] <= '9' && s[1] == 'd') return true;
    return false;
}

typedef struct {
    const FwSyncLeg *leg;
    const FwSyncLive *live;
    const char *msg;
    int page, phase;
} ActiveCtx;

static bool active_text(void *p, const char *el, char *text, size_t cap){
    const ActiveCtx *c = p;
    char f1[FW_FLIGHT_TEXT_MAX], f2[FW_FLIGHT_TEXT_MAX];
    if (!strcmp(el, "heading1")) { fw_sync_ident(c->leg, c->phase, text, cap); return true; }
    if (c->page != 0 && c->page != 4) return false;          /* pages 1-3: as tracked */
    if (strcmp(el, "footer1") && strcmp(el, "footer2")) return false;
    if (c->page == 4) fw_sync_message_footers(c->msg ? c->msg : "", 20, f1, f2, sizeof f1);
    else fw_sync_footers(c->leg, c->live, f1, f2, sizeof f1);
    snprintf(text, cap, "%s", !strcmp(el, "footer1") ? f1 : f2);
    return true;
}

void fw_screen_sync_active(const FwSurface *s, const FwFlightSettings *st, const FwSyncLeg *leg,
                           const FwFlight *live_flight, const FwSyncLive *live, const char *msg, int page,
                           int phase, int64_t now, const FwLogo *logo){
    ActiveCtx c = {leg, live_flight ? live : NULL, msg, page, phase};
    const FwFlight *f = live_flight;
    static FwFlight synth;                                    /* ~0.5 KB, off the stack */
    if (!f) {
        fw_flight_init(&synth);
        fw_sync_flight(leg, synth.cs, sizeof synth.cs);
        snprintf(synth.al_code, sizeof synth.al_code, "%s", leg->al);
        snprintf(synth.orig_iata, sizeof synth.orig_iata, "%s", leg->orig[0] ? leg->orig : "---");
        snprintf(synth.dest_iata, sizeof synth.dest_iata, "%s", leg->dest[0] ? leg->dest : "---");
        f = &synth;
    }
    fw_screen_tracked_ex(s, st, f, page, now, logo, active_text, &c, fw_sync_progress(leg, c.live, now), true);
}

void fw_screen_sync_next(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *legs, int count, int index){
    static const Box HEADER = {4, 4, 120, 8}, FLIGHT = {4, 16, 120, 16}, ROUTE = {4, 34, 120, 16},
                     DEP = {4, 54, 60, 8}, ARR = {68, 54, 56, 8};
    const FwSyncLeg *leg;
    char text[40], part[24];
    begin(s, frame);
    if (count < 1 || !legs) return;
    if (index < 0 || index >= count) index = 0;
    leg = &legs[index];
    if (count < 2) snprintf(text, sizeof text, "UPCOMING");
    else snprintf(text, sizeof text, "UPCOMING %u/%u", (unsigned)(index + 1), (unsigned)count);
    fw_text_draw_bg(s, HEADER.x, HEADER.y, text, CYAN, (FwRgb){0, 0, 0}, 1);
    fw_sync_flight(leg, part, sizeof part);
    fw_text_draw_bg(s, FLIGHT.x, FLIGHT.y, part, WHITE, (FwRgb){0, 0, 0}, 2);
    fw_sync_route(leg, part, sizeof part);
    fw_text_draw_bg(s, ROUTE.x, ROUTE.y, part, WHITE, (FwRgb){0, 0, 0}, 2);
    if (leg->dep_rel[0]) {
        snprintf(text, sizeof text, "Dep %s", fw_sync_strip_in(leg->dep_rel));
        fw_text_draw_bg(s, DEP.x, DEP.y, text, GREEN, (FwRgb){0, 0, 0}, 1);
    }
    if (leg->arr_rel[0] && !digit_then_d(leg->arr_rel)) {
        int tw, x = ARR.x;
        snprintf(text, sizeof text, "Arr %s", fw_sync_strip_in(leg->arr_rel));
        tw = text_w(text, 1);
        if (tw < ARR.w) x += ARR.w - tw;
        fw_text_draw_bg(s, x, ARR.y, text, CYAN, (FwRgb){0, 0, 0}, 1);
    }
}

void fw_screen_sync_list(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *legs, int count){
    static const Box HEADER = {2, 2, 124, 8};
    static const struct { int16_t x, w; bool right; } INDEX = {2, 12, false}, ROUTE = {14, 44, false},
                                                       DEP = {58, 54, false}, FLAG = {112, 14, true};
    char text[48], cell[48];
    begin(s, frame);
    if (count < 1 || !legs) return;
    if (count > FW_SYNC_UPCOMING_MAX) count = FW_SYNC_UPCOMING_MAX;
    if (count == 1) snprintf(text, sizeof text, "UPCOMING");
    else snprintf(text, sizeof text, "UPCOMING - NEXT %u", (unsigned)count);
    fw_sync_fit(text, HEADER.w / FW_FONT_ADVANCE, cell, sizeof cell);
    fw_text_draw_bg(s, HEADER.x, HEADER.y, cell, CYAN, (FwRgb){0, 0, 0}, 1);
    for (int i = 0; i < count; ++i) {
        const FwSyncLeg *leg = &legs[i];
        int y = 10 + 11 * i + 1;
        const char *flag = fw_sync_canceled(leg) ? "X" : strcmp(leg->lt, "deadhead") == 0 ? "DH" : "";
        snprintf(text, sizeof text, "%d", i + 1);
        fw_sync_fit(text, INDEX.w / FW_FONT_ADVANCE, cell, sizeof cell);
        fw_text_draw_bg(s, INDEX.x, y, cell, CYAN, (FwRgb){0, 0, 0}, 1);
        fw_sync_route(leg, text, sizeof text);
        fw_sync_fit(text, ROUTE.w / FW_FONT_ADVANCE, cell, sizeof cell);
        fw_text_draw_bg(s, ROUTE.x, y, cell, WHITE, (FwRgb){0, 0, 0}, 1);
        fw_sync_fit(leg->dep_rel, DEP.w / FW_FONT_ADVANCE, cell, sizeof cell);
        fw_text_draw_bg(s, DEP.x, y, cell, GREEN, (FwRgb){0, 0, 0}, 1);
        fw_sync_fit(flag, FLAG.w / FW_FONT_ADVANCE, cell, sizeof cell);
        if (cell[0]) {
            int tw = text_w(cell, 1), x = FLAG.x + FLAG.w - tw;
            if (x < FLAG.x) x = FLAG.x;
            fw_text_draw_bg(s, x, y, cell, GREY, (FwRgb){0, 0, 0}, 1);
        }
    }
}
