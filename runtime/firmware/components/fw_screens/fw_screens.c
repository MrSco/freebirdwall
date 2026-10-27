#include "fw_screens.h"
#include "fw_qr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const FwRgb white = {255, 255, 255};   /* stock text: constant 0xFFFF */
static const FwRgb black = {0, 0, 0};
static const FwRgb dim = {90, 90, 90};
static const FwRgb green = {0, 255, 0};       /* 0x07E0 */
static const FwRgb bar_grey = {66, 65, 66};   /* 0x4208 */

static void frame(const FwSurface *s, int x, int y, int w, int h, FwRgb c){
    fw_hline(s, x, y, w, c);
    fw_hline(s, x, y + h - 1, w, c);
    fw_vline(s, x, y, h, c);
    fw_vline(s, x + w - 1, y, h, c);
}

/* ------------------------------------------------------------ owned: long text scrolls
 * Stock lets a line wider than its box run off the panel. Here it is drawn
 * clipped to its box and, once the card is up, moved left to show the rest:
 * still for FW_SCROLL_HOLD_MS, then one pixel every FW_SCROLL_PX_MS until its
 * end shows, still again for FW_SCROLL_END_MS, then back to the start. Only
 * that line is redrawn, in opaque cells, so nothing flickers on the
 * single-buffered panel. Lines that fit are drawn as stock. */
typedef struct {
    FwElement e;
    FwRgb color;
    int width, offset;
    char text[FW_FLIGHT_TEXT_MAX];
} Scroll;
static Scroll s_scroll[FW_SCROLL_LINES];
static int s_scroll_n;

static int s_px_ms = FW_SCROLL_PX_MS;

void fw_screen_scroll_reset(void){ s_scroll_n = 0; }
void fw_screen_scroll_speed(int px_ms){ s_px_ms = px_ms < 10 ? 10 : px_ms > 200 ? 200 : px_ms; }

uint32_t fw_screen_scroll_pass_ms(void){
    uint32_t most = 0;
    int i;
    for (i = 0; i < s_scroll_n; ++i) {
        int over = s_scroll[i].width - s_scroll[i].e.w;
        uint32_t t = FW_SCROLL_HOLD_MS + (uint32_t)(over > 0 ? over : 0) * (uint32_t)s_px_ms + FW_SCROLL_END_MS;
        if (t > most) most = t;
    }
    return most;
}
int fw_screen_scroll_count(void){ return s_scroll_n; }

int fw_scroll_offset(int text_w, int box_w, uint32_t ms){
    int over = text_w - box_w;
    uint32_t move, t;
    if (over <= 0) return 0;
    move = (uint32_t)over * (uint32_t)s_px_ms;
    t = ms % (FW_SCROLL_HOLD_MS + move + FW_SCROLL_END_MS);
    if (t < FW_SCROLL_HOLD_MS) return 0;
    t -= FW_SCROLL_HOLD_MS;
    return t < move ? (int)(t / (uint32_t)s_px_ms) : over;
}

typedef struct { const FwSurface *s; int x0, x1; } Clip;

static void clip_pixel(void *ctx, int x, int y, FwRgb c){
    const Clip *k = (const Clip *)ctx;
    if (x >= k->x0 && x < k->x1) k->s->pixel(k->s->ctx, x, y, c);
}
static void clip_fill(void *ctx, int x, int y, int w, int h, FwRgb c){
    const Clip *k = (const Clip *)ctx;
    if (x < k->x0) { w -= k->x0 - x; x = k->x0; }
    if (x + w > k->x1) w = k->x1 - x;
    if (w > 0) fw_fill_rect(k->s, x, y, w, h, c);
}

/* The line from offset pixels in, clipped to its box; what is left of the
 * box after the text is cleared. */
static void draw_clipped(const FwSurface *s, const FwElement *e, const char *text, FwRgb color, int width, int offset){
    Clip k = {s, e->x < 0 ? 0 : e->x, e->x + e->w < s->width ? e->x + e->w : s->width};
    FwSurface c = {&k, s->width, s->height, clip_pixel, clip_fill};
    int y = e->y + (e->h - FW_FONT_LINE) / 2, end = e->x - offset + width;
    fw_text_draw_bg(&c, e->x - offset, y, text, color, black, 1);
    if (end < k.x1) fw_fill_rect(s, end, y, k.x1 - end, FW_FONT_LINE, black);
}

bool fw_screen_scroll_tick(const FwSurface *s, uint32_t ms){
    bool moved = false;
    int i;
    for (i = 0; i < s_scroll_n; ++i) {
        Scroll *l = &s_scroll[i];
        int off = fw_scroll_offset(l->width, l->e.w, ms);
        if (off == l->offset) continue;
        l->offset = off;
        draw_clipped(s, &l->e, l->text, l->color, l->width, off);
        moved = true;
    }
    return moved;
}

/* Every full screen starts here: what scrolled belongs to the last one. */
static void begin(const FwSurface *s){
    s_scroll_n = 0;
    fw_clear(s);
}

/* From the vendor's mini-v1 tables (vendor-layouts.json). */
const FwElement fw_layout_mini_default[6] = {
    {"logo",     FW_EL_IMAGE,  2,  2,  36, 36, false},
    {"heading1", FW_EL_TEXT,  40,  2,  86, 12, false},
    {"heading2", FW_EL_TEXT,  40, 14,  86, 12, false},
    {"heading3", FW_EL_TEXT,  40, 26,  86, 12, false},
    {"footer1",  FW_EL_TEXT,   2, 40, 124, 10, false},
    {"footer2",  FW_EL_TEXT,   2, 52, 124, 10, false},
};

static const FwElement layout_mini_no_logo[5] = {
    {"heading1", FW_EL_TEXT,   2,  2, 124, 12, false},
    {"heading2", FW_EL_TEXT,   2, 14, 124, 12, false},
    {"heading3", FW_EL_TEXT,   2, 26, 124, 12, false},
    {"footer1",  FW_EL_TEXT,   2, 40, 124, 10, false},
    {"footer2",  FW_EL_TEXT,   2, 52, 124, 10, false},
};

const FwElement *fw_layout_mini(const char *name, int *count){
    if (name && strcmp(name, "NO_LOGO") == 0) { *count = 5; return layout_mini_no_logo; }
    /* TECHNICAL and PLUS_SYNC_ACTIVE_MAIN share DEFAULT's geometry. */
    *count = 6;
    return fw_layout_mini_default;
}

void fw_screen_element_text(const FwSurface *s, const FwElement *e, const char *text, FwRgb color){
    int w = fw_text_glyph_count(text) * FW_FONT_ADVANCE;
    int x = e->right ? e->x + e->w - w : e->x;
    if (!e->right && w > e->w) {                 /* owned: too wide, so clipped, and it scrolls */
        if (s_scroll_n < FW_SCROLL_LINES) {
            Scroll *l = &s_scroll[s_scroll_n++];
            l->e = *e;
            l->color = color;
            l->width = w;
            l->offset = 0;
            snprintf(l->text, sizeof l->text, "%s", text);
        }
        draw_clipped(s, e, text, color, w, 0);
        return;
    }
    int y = e->y + (e->h - FW_FONT_LINE) / 2;
    fw_text_draw_bg(s, x, y, text, color, black, 1);
}

static const FwElement *layout_for(const FwFlightSettings *st, int *count){
    return fw_layout_mini(st->layout, count);
}

void fw_screen_flight(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, const FwLogo *logo){
    char text[FW_FLIGHT_TEXT_MAX];
    int count, i;
    const FwElement *els = layout_for(st, &count);
    begin(s);
    for (i = 0; i < count; ++i) {
        const FwElement *e = &els[i];
        if (e->type == FW_EL_IMAGE) {
            if (logo) fw_logo_draw(s, e->x, e->y, e->w, e->h, logo);
        } else if (fw_flight_text(st, f, e->name, e->w, text, sizeof text)) {
            fw_screen_element_text(s, e, text, white);
        }
    }
    if (st->progress && f->prog == f->prog) fw_screen_progress(s, f->prog);   /* owned: below footer2 */
}

/* FUN_4200b92c */
void fw_screen_progress(const FwSurface *s, double prog){
    int len = s->width - 4, fill;
    fw_hline(s, 2, s->height - 2, len, bar_grey);
    if (prog != prog) return;   /* NAN: no fill */
    if (prog < 0.0) prog = 0.0;
    else if (prog > 100.0) prog = 100.0;
    fill = (int)(prog / 100.0 * (double)len);
    if (fill > 0) fw_hline(s, 2, s->height - 2, fill, green);
}

void fw_screen_tracked_ex(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, int page, int64_t now,
                          const FwLogo *logo, FwTextOverride override, void *ctx, double progress, bool nan_black){
    char text[FW_FLIGHT_TEXT_MAX];
    FwTrackState t;
    int count, i;
    const FwElement *els = layout_for(st, &count);
    fw_track_state(f, &t);
    begin(s);
    for (i = 0; i < count; ++i) {
        const FwElement *e = &els[i];
        if (e->type == FW_EL_IMAGE) {
            if (logo) fw_logo_draw(s, e->x, e->y, e->w, e->h, logo);
        } else if (override && override(ctx, e->name, text, sizeof text)) {
            if (text[0]) fw_screen_element_text(s, e, text, white);
        } else if (fw_track_text(st, f, &t, page, e->name, e->w, now, text, sizeof text)) {
            bool landed_line = t.landed && strcmp(e->name, "footer2") == 0;
            fw_screen_element_text(s, e, text, landed_line ? green : white);
        }
    }
    if (progress != progress && nan_black) fw_hline(s, 2, s->height - 2, s->width - 4, black);
    else fw_screen_progress(s, progress);
}

void fw_screen_tracked(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, int page,
                       int64_t now, const FwLogo *logo){
    fw_screen_tracked_ex(s, st, f, page, now, logo, NULL, NULL, f->has_trk ? f->prog : NAN, false);
}

/* ------------------------------------------------------------ frame */

static const FwRgb orange = {255, 130, 0};    /* 0xFC00 */

static uint8_t from5(unsigned v){ return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t from6(unsigned v){ return (uint8_t)((v << 2) | (v >> 4)); }

/* FUN_4201a900: grey at the border brightness, as RGB565. */
static FwRgb border_grey(uint8_t percent){
    unsigned r = percent * 31u / 100u, g = percent * 63u / 100u;
    FwRgb c;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    c.r = from5(r); c.g = from6(g); c.b = from5(r);
    return c;
}

void fw_screen_frame(const FwSurface *s, const FwFrame *fr){
    static const FwFrame none = {false, 0, false};
    FwRgb grey;
    if (!fr) fr = &none;
    grey = border_grey(fr->border_percent);
    if (fr->border) {
        fw_hline(s, 0, 0, s->width, grey);
        fw_hline(s, 0, s->height - 1, s->width, grey);
        fw_vline(s, 0, 0, s->height, grey);
        fw_vline(s, s->width - 1, 0, s->height, grey);
    }
    /* FUN_4201a93c */
    if (fr->indicator) {
        fw_fill_rect(s, s->width - 2, 0, 2, 2, orange);
    } else {
        fw_fill_rect(s, s->width - 2, 0, 2, 2, fr->border ? grey : black);
        if (fr->border) fw_fill_rect(s, s->width - 2, 1, 1, 1, black);
    }
}

void fw_screen_too_many(const FwSurface *s, const FwFrame *fr, int seeing){
    char line[32];
    begin(s);
    snprintf(line, sizeof line, "Seeing %d flights", seeing);
    fw_screen_frame(s, fr);
    fw_text_draw_bg(s, 3, 3, "Too many flights,", orange, black, 1);
    fw_text_draw_bg(s, 3, 15, "reduce area or", orange, black, 1);
    fw_text_draw_bg(s, 3, 27, "add filters", orange, black, 1);
    fw_text_draw_bg(s, 3, 52, line, orange, black, 1);
}

void fw_screen_message(const FwSurface *s, const FwFrame *fr, const char *text){
    int w = fw_text_glyph_count(text) * FW_FONT_ADVANCE;
    begin(s);
    fw_screen_frame(s, fr);
    fw_text_draw(s, s->width - w >= -1 ? (s->width - w) / 2 : 0, 28, text, white, 1);
}

/* ------------------------------------------------------------ setup */

void fw_screen_lines(const FwSurface *s, const FwFrame *fr, const char *l1, const char *l2, const char *l3,
                     FwRgb color){
    begin(s);
    fw_screen_frame(s, fr);
    if (l1) fw_text_draw(s, 3, 3, l1, color, 1);
    if (l2 && *l2) fw_text_draw(s, 3, 15, l2, color, 1);
    if (l3 && *l3) fw_text_draw(s, 3, 27, l3, color, 1);
}

int fw_splash_logo_h(int height){
    return (int)((double)(2 * height) * 0.9 / 3.0);
}

/* Owned: the FreeBirdWall mark (tools/make_icon.py, fw_brand.c). */
extern const int fw_brand_bird_w, fw_brand_bird_h;
extern const uint8_t fw_brand_bird[];

void fw_screen_brand(const FwSurface *s, int top, int box_h){
    static const FwRgb accent = {0, 200, 255};          /* the Freebird app's accent, #00C8FF */
    const int gap = 6, text_w = 12 * FW_FONT_ADVANCE;   /* "FreeBirdWall" */
    int x0 = (s->width - (fw_brand_bird_w + gap + text_w)) / 2, y0 = top + (box_h - fw_brand_bird_h) / 2;
    int x, y, tx, ty;
    for (y = 0; y < fw_brand_bird_h; ++y)
        for (x = 0; x < fw_brand_bird_w; ++x) {
            int i = y * fw_brand_bird_w + x;
            int v = (fw_brand_bird[i >> 1] >> (i & 1 ? 0 : 4)) & 15;   /* coverage 0..15 */
            if (v) {
                FwRgb c = {(uint8_t)(v * 17), (uint8_t)(v * 17), (uint8_t)(v * 17)};
                fw_fill_rect(s, x0 + x, y0 + y, 1, 1, c);
            }
        }
    tx = x0 + fw_brand_bird_w + gap;
    ty = top + (box_h - FW_FONT_LINE) / 2;
    fw_text_draw(s, tx, ty, "FreeBird", white, 1);
    fw_text_draw(s, tx + 8 * FW_FONT_ADVANCE, ty, "Wall", accent, 1);
}

void fw_screen_splash(const FwSurface *s, FwSplash *st, const char *const *lines, int n, FwRgb color){
    static const FwRgb black = {0, 0, 0};
    char key[sizeof st->key];
    size_t len = 0;
    int logo_h = fw_splash_logo_h(s->height), y;
    bool white = color.r == 255 && color.g == 255 && color.b == 255;
    if (!st->logo_drawn) {
        begin(s);
        fw_screen_brand(s, 8, logo_h);                  /* stock drew the vendor's logo here */
        st->logo_drawn = true;
        st->key[0] = 0;
    }
    key[0] = 0;
    for (int i = 0; i < n; ++i)
        len += (size_t)snprintf(key + len, len < sizeof key ? sizeof key - len : 0, "%s%s", i ? "\n" : "",
                                lines[i] ? lines[i] : "");
    if (strcmp(key, st->key) == 0) return;                 /* unchanged: nothing is drawn */
    snprintf(st->key, sizeof st->key, "%s", key);
    fw_fill_rect(s, 0, logo_h + 8, s->width, s->height - (logo_h + 8), black);
    y = logo_h + 11;
    for (int i = 0; i < n; ++i, y += FW_FONT_LINE + 2) {
        const char *t = lines[i];
        int tw, x;
        if (!t || !*t) continue;
        tw = fw_text_glyph_count(t) * FW_FONT_ADVANCE;
        x = s->width - tw >= -1 ? (s->width - tw) / 2 : 0;
        if (white) fw_text_draw(s, x, y, t, color, 1);
        else fw_text_draw_bg(s, x, y, t, color, black, 1);
    }
}

static void escape_join(const char *in, char *out, size_t cap){
    size_t n = 0;
    for (; *in && n + 2 < cap; ++in) {
        if (strchr("\\;,:\"", *in)) out[n++] = '\\';
        out[n++] = *in;
    }
    out[n] = 0;
}

bool fw_setup_info(const char *id, const char *fp, FwSetupInfo *out){
    memset(out, 0, sizeof *out);
    if (!id || !*id) return false;
    if (!fp) fp = "";
    snprintf(out->ssid, sizeof out->ssid, "Flightwall-%s", id);
    snprintf(out->password, sizeof out->password, "%.8s", fp);
    snprintf(out->qr, sizeof out->qr, "FW2;ID=%s;FP=%s", id, fp);
    {
        /* WIFI: join code; \ ; , : " are escaped (the stock alphabets never
         * produce them, but the escaping keeps the code valid regardless). */
        char ssid[64], pass[48];
        escape_join(out->ssid, ssid, sizeof ssid);
        escape_join(out->password, pass, sizeof pass);
        snprintf(out->join_qr, sizeof out->join_qr, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    }
    return strlen(out->password) >= 8;
}

/* The stock QR drawing (FUN_4201ac4c): doubled when it fits in 60 px, dark
 * modules white. x0/y0 < 0: centred on the panel. */
static void draw_qr(const FwSurface *s, const char *text, int x0, int y0){
    static uint8_t modules[FW_QR_MAX_SIZE * FW_QR_MAX_SIZE];
    int size, scale, span, x, y;
    size = fw_qr_v3_low(text, modules);
    scale = size * 2 > 60 ? 1 : 2;
    span = size * scale;
    if (x0 < 0) x0 = (s->width - span) / 2;
    if (y0 < 0) y0 = (s->height - span) / 2;
    for (y = 0; y < size; ++y)
        for (x = 0; x < size; ++x)
            fw_fill_rect(s, x0 + x * scale, y0 + y * scale, scale, scale, modules[y * size + x] ? white : black);
}

static void setup_qr(const FwSurface *s, const FwFrame *fr, const char *text){
    begin(s);
    fw_screen_frame(s, fr);
    draw_qr(s, text, -1, -1);
}

void fw_screen_pair_code(const FwSurface *s, const char *code, int seconds_left){
    static const FwRgb cyan = {0, 255, 255}, grey = {150, 150, 150};
    char left[24];
    int w;
    begin(s);
    w = fw_text_glyph_count("Dashboard code") * FW_FONT_ADVANCE;
    fw_text_draw(s, (s->width - w) / 2, 6, "Dashboard code", cyan, 1);
    w = fw_text_glyph_count(code) * FW_FONT_ADVANCE * 2;
    fw_text_draw(s, (s->width - w) / 2, 22, code, white, 2);
    if (seconds_left < 0) seconds_left = 0;
    if (seconds_left > 5999) seconds_left = 5999;
    snprintf(left, sizeof left, "%d:%02d left", seconds_left / 60, seconds_left % 60);
    w = fw_text_glyph_count(left) * FW_FONT_ADVANCE;
    fw_text_draw(s, (s->width - w) / 2, 50, left, grey, 1);
}

void fw_screen_play(const FwSurface *s, const FwFrame *fr, const char *url){
    static const FwRgb cyan = {0, 255, 255};
    begin(s);
    fw_screen_frame(s, fr);
    draw_qr(s, url, 3, 3);
    fw_text_draw(s, 66, 6, "Scan to", white, 1);
    fw_text_draw(s, 66, 16, "play", white, 1);
    fw_text_draw(s, 66, 32, "Pong", cyan, 1);
    fw_text_draw(s, 66, 42, "Breakout", cyan, 1);
}

void fw_screen_dashboard_qr(const FwSurface *s, const FwFrame *fr, const char *url){
    static const FwRgb cyan = {0, 255, 255};
    char host[24], port[8] = "";
    const char *h = strstr(url, "://"), *colon;
    size_t n;
    h = h ? h + 3 : url;
    colon = strchr(h, ':');
    n = colon ? (size_t)(colon - h) : strcspn(h, "/");
    if (n >= sizeof host) n = sizeof host - 1;
    memcpy(host, h, n);
    host[n] = 0;
    if (colon) snprintf(port, sizeof port, ":%.*s", (int)strcspn(colon + 1, "/"), colon + 1);
    begin(s);
    fw_screen_frame(s, fr);
    draw_qr(s, url, 3, 3);
    fw_text_draw(s, 66, 4, "Scan for", white, 1);
    fw_text_draw(s, 66, 14, "settings", white, 1);
    if (strlen(host) <= 10) {                  /* 10 characters fit beside the QR */
        fw_text_draw(s, 66, 32, host, cyan, 1);
        fw_text_draw(s, 66, 42, port, cyan, 1);
    } else {                                   /* "192.168" / ".1.50" / ":8080": split at a dot */
        char first[16];
        size_t cut = 0, i;
        for (i = 1; host[i] && i <= 10; ++i) if (host[i] == '.') cut = i;
        if (!cut) cut = 10;
        snprintf(first, sizeof first, "%.*s", (int)cut, host);
        fw_text_draw(s, 66, 28, first, cyan, 1);
        fw_text_draw(s, 66, 38, host + cut, cyan, 1);
        fw_text_draw(s, 66, 48, port, cyan, 1);
    }
}

void fw_screen_setup(const FwSurface *s, const FwFrame *fr, int page, const FwSetupInfo *info, bool reconnecting){
    static const FwRgb amber = {255, 166, 0};   /* 0xFD20 */
    switch (page) {
    case 0:
        if (reconnecting) fw_screen_lines(s, fr, "Attempting to", "reconnect...", "", amber);
        else fw_screen_lines(s, fr, "Scan QR or type", "credentials to", "begin setup", white);
        break;
    case 1:
        setup_qr(s, fr, info->qr);
        break;
    case FW_SETUP_PAGE_JOIN_QR:
        setup_qr(s, fr, info->join_qr);
        break;
    default:
        begin(s);
        fw_screen_frame(s, fr);
        fw_text_draw(s, 3, 3, "SSID", white, 1);
        fw_text_draw(s, 3, 15, info->ssid, white, 1);
        fw_text_draw(s, 3, 29, "Password", white, 1);
        fw_text_draw(s, 3, 41, info->password, white, 1);
        break;
    }
}

/* ------------------------------------------------------------ samples */

static void sample_flight(FwFlight *f){
    fw_flight_init(f);
    strcpy(f->cs, "AAL1234");
    strcpy(f->reg, "N915NN");
    strcpy(f->al_code, "AAL");
    strcpy(f->al_name, "American");
    strcpy(f->orig_iata, "CLT");
    strcpy(f->dest_iata, "PBI");
    strcpy(f->orig_icao, "KCLT");
    strcpy(f->dest_icao, "KPBI");
    strcpy(f->orig, "Charlotte Douglas");
    strcpy(f->dest, "Palm Beach Intl");
    strcpy(f->ac_name, "737-800");
}

void fw_screen_flight_card(const FwSurface *s, const FwLogo *logo){
    static const FwFlightSettings st = {{0, 0, 0, 0, false}, FW_TITLE_AIRLINE, NULL, 0, true, "DEFAULT", NULL, NULL, false};
    const FwElement *box = &fw_layout_mini_default[0];
    FwFlight f;
    sample_flight(&f);
    fw_screen_flight(s, &st, &f, logo);
    if (!logo) {
        int side = fw_logo_side_for_box(box->w, box->h);
        int x = box->x + (box->w - side) / 2, y = box->y + (box->h - side) / 2;
        frame(s, x, y, side, side, dim);
        fw_text_draw(s, x + (side - 23) / 2, y + (side - FW_FONT_LINE) / 2, "LOGO", dim, 1);
    }
}

void fw_screen_text_test(const FwSurface *s){
    begin(s);
    fw_text_draw(s, 2, 1, "Z\xC3\xBCrich Montr\xC3\xA9" "al", white, 1);          /* ü é: CP437 glyphs */
    fw_text_draw(s, 2, 10, "S\xC3\xA3o Paulo Krak\xC3\xB3w", white, 1);           /* ã: vendor glyph, ó: CP437 */
    fw_text_draw(s, 2, 19, "\xC5\x81" "\xC3\xB3" "d\xC5\xBA \xC5\xBD" "ilina", white, 1); /* Ł ź Ž: vendor glyphs */
    fw_text_draw(s, 2, 28, "\xE2\x82\xAC 5 \xE2\x98\x85 \xE2\x9C\x88 \xC2\xB0", white, 1); /* € ★ ✈ ° */
    fw_text_draw(s, 2, 40, "CLT-PBI", white, 2);
}

void fw_screen_bars(const FwSurface *s){
    static const FwRgb bars[8] = {
        {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
        {0, 255, 255},   {255, 0, 255}, {255, 255, 0}, {96, 96, 96},
    };
    int i;
    begin(s);
    frame(s, 0, 0, s->width, s->height, white);
    for (i = 0; i < 8; ++i) fw_fill_rect(s, 4 + i * 15, 6, 13, 46, bars[i]);
}
