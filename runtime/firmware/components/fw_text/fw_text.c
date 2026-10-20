#include "fw_text.h"
#include "vendor_charmap.h"

#define MAX_SCALE 4

/* ------------------------------------------------------------ code points */

/* Same lookups as the stock FUN_4201bae8 / FUN_4201baac: binary searches over
 * tables sorted by code point. */
static int charmap_glyph(uint32_t cp){
    int lo = 0, hi = FW_VENDOR_CHARMAP_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fw_vendor_charmap[mid].cp == cp) return fw_vendor_charmap[mid].glyph;
        if (fw_vendor_charmap[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int extra_glyph(uint32_t cp){
    int lo = 0, hi = FW_VENDOR_EXTRA_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fw_vendor_extra_glyphs[mid].cp == cp) return FW_GLYPH_EXTRA_FIRST + mid;
        if (fw_vendor_extra_glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int fw_text_glyph_for(uint32_t cp){
    int g;
    if (cp < 0x80) return (int)cp;
    if (cp <= 0xFFFF && (g = charmap_glyph(cp)) >= 0) return g;
    if ((g = extra_glyph(cp)) >= 0) return g;
    return '?';
}

/* Same decoding as the stock FUN_420d9378, malformed cases included: a stray
 * continuation or invalid lead byte is one '?'; a bad continuation byte makes
 * the lead one '?' and decoding resumes at the next byte; a sequence cut short
 * by the end of the text is one '?' that consumes the rest. There are no
 * overlong or surrogate checks (C0 AF decodes to '/'), matching stock. */
int32_t fw_text_next_codepoint(const char **s, const char *end){
    const unsigned char *p = (const unsigned char *)*s;
    const unsigned char *e = (const unsigned char *)end;
    uint32_t cp;
    int need, i, rem = 0;
    if (!p || (e && p >= e) || !*p) return FW_TEXT_END;
    while (rem < 4 && (!e || p + rem < e) && p[rem]) ++rem;   /* bytes left, as far as a sequence reaches */
    if (p[0] < 0x80) { *s = (const char *)(p + 1); return p[0]; }
    if ((p[0] & 0xE0) == 0xC0) { need = 2; cp = p[0] & 0x1Fu; }
    else if ((p[0] & 0xF0) == 0xE0) { need = 3; cp = p[0] & 0x0Fu; }
    else if ((p[0] & 0xF8) == 0xF0) { need = 4; cp = p[0] & 0x07u; }
    else { *s = (const char *)(p + 1); return FW_TEXT_MALFORMED; }   /* stray continuation / invalid lead */
    if (need > rem) { *s = (const char *)(p + rem); return FW_TEXT_MALFORMED; }
    for (i = 1; i < need; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *s = (const char *)(p + 1); return FW_TEXT_MALFORMED; }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *s = (const char *)(p + need);
    return (int32_t)cp;
}

int fw_text_next_glyph(const char **s, const char *end){
    int32_t cp = fw_text_next_codepoint(s, end);
    if (cp == FW_TEXT_END) return -1;
    return cp == FW_TEXT_MALFORMED ? '?' : fw_text_glyph_for((uint32_t)cp);
}

/* ------------------------------------------------------------ drawing */

static int clamp_scale(int scale){ return scale < 1 ? 1 : scale > MAX_SCALE ? MAX_SCALE : scale; }

static uint8_t rows_column(const uint8_t rows[8], int col){
    uint8_t bits = 0;
    int row;
    for (row = 0; row < 8; ++row)
        if (rows[row] & (0x10 >> col)) bits = (uint8_t)(bits | 1u << row);
    return bits;
}

static uint8_t column(int glyph, int col){
    if (glyph == FW_GLYPH_ELLIPSIS) return (col % 2 == 0) ? 0x40 : 0x00; /* dots on row 6, cols 0,2,4 */
    if (glyph >= FW_GLYPH_EXTRA_FIRST)
        return rows_column(fw_vendor_extra_glyphs[glyph - FW_GLYPH_EXTRA_FIRST].rows, col);
    return fw_font5x7[glyph * 5 + col];
}

/* Lit pixels only, or with bg the whole 6x8 cell (glyph + gap column), each
 * pixel written once - the stock firmware's opaque text. */
static void draw_columns(const FwSurface *sf, int x, int y, const uint8_t bits_by_col[FW_FONT_W],
                         FwRgb color, const FwRgb *bg, int scale){
    int col, row, dx, dy;
    if (bg) {
        for (col = 0; col < FW_FONT_ADVANCE; ++col) {
            uint8_t bits = col < FW_FONT_W ? bits_by_col[col] : 0;
            for (row = 0; row < FW_FONT_LINE; ++row)
                fw_fill_rect(sf, x + col * scale, y + row * scale, scale, scale, (bits >> row) & 1 ? color : *bg);
        }
        return;
    }
    for (col = 0; col < FW_FONT_W; ++col) {
        uint8_t bits = bits_by_col[col];
        for (row = 0; bits; ++row, bits >>= 1) {
            if (!(bits & 1)) continue;
            for (dy = 0; dy < scale; ++dy) {
                int py = y + row * scale + dy;
                if (py < 0 || py >= sf->height) continue;
                for (dx = 0; dx < scale; ++dx) {
                    int px = x + col * scale + dx;
                    if (px >= 0 && px < sf->width) sf->pixel(sf->ctx, px, py, color);
                }
            }
        }
    }
}

static void draw_glyph_bg(const FwSurface *sf, int x, int y, int glyph, FwRgb color, const FwRgb *bg, int scale){
    uint8_t cols[FW_FONT_W];
    int col;
    for (col = 0; col < FW_FONT_W; ++col) cols[col] = column(glyph, col);
    draw_columns(sf, x, y, cols, color, bg, scale);
}

static void draw_glyph(const FwSurface *sf, int x, int y, int glyph, FwRgb color, int scale){
    draw_glyph_bg(sf, x, y, glyph, color, NULL, scale);
}

void fw_text_draw_rows(const FwSurface *sf, int x, int y, const uint8_t rows[8], FwRgb color,
                       const FwRgb *bg, int scale){
    uint8_t cols[FW_FONT_W];
    int col;
    if (!sf || !sf->pixel || !rows) return;
    for (col = 0; col < FW_FONT_W; ++col) cols[col] = rows_column(rows, col);
    draw_columns(sf, x, y, cols, color, bg, clamp_scale(scale));
}

static int count_glyphs(const char *utf8){
    const char *p = utf8;
    int n = 0;
    while (fw_text_next_glyph(&p, NULL) >= 0) ++n;
    return n;
}

static int width_of(int glyphs, int scale){
    return glyphs > 0 ? glyphs * FW_FONT_ADVANCE * scale - scale : 0;
}

int fw_text_width(const char *utf8, int scale){
    return utf8 ? width_of(count_glyphs(utf8), clamp_scale(scale)) : 0;
}

int fw_text_draw(const FwSurface *sf, int x, int y, const char *utf8, FwRgb color, int scale){
    const char *p = utf8;
    int glyph;
    scale = clamp_scale(scale);
    if (!sf || !sf->pixel || !utf8) return x;
    while ((glyph = fw_text_next_glyph(&p, NULL)) >= 0) {
        draw_glyph(sf, x, y, glyph, color, scale);
        x += FW_FONT_ADVANCE * scale;
    }
    return x;
}

int fw_text_draw_bg(const FwSurface *sf, int x, int y, const char *utf8, FwRgb color, FwRgb bg, int scale){
    const char *p = utf8;
    int glyph;
    scale = clamp_scale(scale);
    if (!sf || !sf->pixel || !utf8) return x;
    while ((glyph = fw_text_next_glyph(&p, NULL)) >= 0) {
        draw_glyph_bg(sf, x, y, glyph, color, &bg, scale);
        x += FW_FONT_ADVANCE * scale;
    }
    return x;
}

int fw_text_glyph_count(const char *utf8){ return utf8 ? count_glyphs(utf8) : 0; }

int fw_text_draw_fit(const FwSurface *sf, int x, int y, int max_width, const char *utf8,
                     FwRgb color, int scale, FwOverflow overflow, bool *truncated){
    const char *p = utf8;
    int total, keep, glyph, drawn = 0, i, last_non_space;
    const char *q;
    if (truncated) *truncated = false;
    scale = clamp_scale(scale);
    if (!sf || !sf->pixel || !utf8 || max_width <= 0) return 0;
    total = count_glyphs(utf8);
    if (width_of(total, scale) <= max_width) {
        fw_text_draw(sf, x, y, utf8, color, scale);
        return width_of(total, scale);
    }
    if (truncated) *truncated = true;
    if (overflow == FW_TEXT_ELLIPSIS) {
        /* keep glyphs + 1 ellipsis glyph must fit */
        keep = (max_width + scale) / (FW_FONT_ADVANCE * scale) - 1;
        if (keep < 0) return 0;
        /* drop trailing spaces before the ellipsis */
        last_non_space = -1;
        for (i = 0, q = utf8; i < keep && (glyph = fw_text_next_glyph(&q, NULL)) >= 0; ++i)
            if (glyph != ' ') last_non_space = i;
        keep = last_non_space + 1;
    } else {
        keep = (max_width + scale) / (FW_FONT_ADVANCE * scale);
    }
    for (i = 0; i < keep && (glyph = fw_text_next_glyph(&p, NULL)) >= 0; ++i) {
        draw_glyph(sf, x + drawn, y, glyph, color, scale);
        drawn += FW_FONT_ADVANCE * scale;
    }
    if (overflow == FW_TEXT_ELLIPSIS) {
        draw_glyph(sf, x + drawn, y, FW_GLYPH_ELLIPSIS, color, scale);
        drawn += FW_FONT_ADVANCE * scale;
    }
    return drawn > 0 ? drawn - scale : 0;
}

int fw_text_center_x(int left, int span, int text_width){
    return left + (span - text_width) / 2;
}

/* ------------------------------------------------------------ primitives */

void fw_fill_rect(const FwSurface *sf, int x, int y, int w, int h, FwRgb color){
    int x1, y1, px, py;
    if (!sf || !sf->pixel || w <= 0 || h <= 0) return;
    x1 = x + w; y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sf->width) x1 = sf->width;
    if (y1 > sf->height) y1 = sf->height;
    if (x >= x1 || y >= y1) return;
    if (sf->fill) { sf->fill(sf->ctx, x, y, x1 - x, y1 - y, color); return; }
    for (py = y; py < y1; ++py)
        for (px = x; px < x1; ++px) sf->pixel(sf->ctx, px, py, color);
}

void fw_hline(const FwSurface *sf, int x, int y, int w, FwRgb color){ fw_fill_rect(sf, x, y, w, 1, color); }
void fw_vline(const FwSurface *sf, int x, int y, int h, FwRgb color){ fw_fill_rect(sf, x, y, 1, h, color); }

void fw_clear(const FwSurface *sf){
    static const FwRgb black = {0, 0, 0};
    if (sf) fw_fill_rect(sf, 0, 0, sf->width, sf->height, black);
}
