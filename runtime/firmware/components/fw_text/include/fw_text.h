#ifndef FLIGHTWALL_FW_TEXT_H
#define FLIGHTWALL_FW_TEXT_H
/* Text rendering for the 128x64 panel with the stock firmware's font.
 *
 * The font is the classic 5x7 Adafruit table, byte-identical to the one in the
 * vendor firmware, so text looks exactly like the stock display: glyph cells
 * are 5 px wide and 8 rows tall (7 + descender row), advanced 6 px, lines 8 px.
 *
 * Input is UTF-8, mapped to glyphs exactly as the stock firmware does
 * (FUN_4201a5c8): code points below 0x80 draw that font glyph; others use the
 * vendor's 134-entry map into the CP437 font (Zürich, Montréal keep their
 * accents), then its 63 extra hand-drawn glyphs (São, Kraków, €, ★, ✈);
 * anything else, and malformed UTF-8, draws '?'. Malformed input never reads
 * past the end of the string.
 *
 * Portable C with no SDK dependency: the board draws through a pixel callback,
 * desktop tests through a framebuffer. All drawing is clipped to the surface. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_FONT_W 5        /* glyph columns */
#define FW_FONT_H 8        /* glyph rows, including the descender row */
#define FW_FONT_ADVANCE 6  /* glyph + 1 px gap */
#define FW_FONT_LINE 8

/* Glyph codes 0..255 are font glyphs; 256 is a compact one-cell ellipsis (three
 * single-pixel dots) used only when text is cut to fit; 257.. are the vendor's
 * extra glyphs, in table order. */
#define FW_GLYPH_ELLIPSIS 256
#define FW_GLYPH_EXTRA_FIRST 257

extern const uint8_t fw_font5x7[256 * 5];

typedef struct { uint8_t r, g, b; } FwRgb;

/* A 5x8 glyph stored as rows, top first; bit 4 is the left column. The vendor's
 * extra glyphs and the clock's date glyphs use this form. */
typedef struct { uint32_t cp; uint8_t rows[8]; } FwRowGlyph;

typedef struct {
    void *ctx;
    int width, height;
    void (*pixel)(void *ctx, int x, int y, FwRgb color); /* only called in bounds */
    /* Optional fast fill of an in-bounds rectangle; NULL = per-pixel fallback. */
    void (*fill)(void *ctx, int x, int y, int w, int h, FwRgb color);
} FwSurface;

/* Rectangle primitives, clipped to the surface. */
void fw_fill_rect(const FwSurface *surface, int x, int y, int w, int h, FwRgb color);
void fw_hline(const FwSurface *surface, int x, int y, int w, FwRgb color);
void fw_vline(const FwSurface *surface, int x, int y, int h, FwRgb color);
void fw_clear(const FwSurface *surface);

typedef enum {
    FW_TEXT_CLIP,      /* draw only whole glyphs that fit */
    FW_TEXT_ELLIPSIS,  /* cut at a glyph boundary and end with the ellipsis */
} FwOverflow;

#define FW_TEXT_END (-2)        /* no more text */
#define FW_TEXT_MALFORMED (-1)  /* malformed UTF-8: draws '?' */

/* Decodes the next code point at *s (stopping at end or NUL) exactly as the
 * stock firmware does, advancing *s past it. */
int32_t fw_text_next_codepoint(const char **s, const char *end);

/* Same, but returns the glyph code to draw ('?' for malformed input), or -1
 * when the text is exhausted. */
int fw_text_next_glyph(const char **s, const char *end);

/* Glyph for one Unicode code point (exposed for tests). */
int fw_text_glyph_for(uint32_t codepoint);

/* Width in pixels of the whole string at the given scale (1..4): glyphs x 6 x
 * scale, minus the trailing 1 px gap. Empty text is 0 wide. */
int fw_text_width(const char *utf8, int scale);

/* Draws at (x, y) = top-left of the first glyph cell. Returns the pen x after
 * the text (start of where the next glyph would go). */
int fw_text_draw(const FwSurface *surface, int x, int y, const char *utf8, FwRgb color, int scale);

/* Draws within max_width pixels. Sets *truncated (if non-NULL) when the text
 * did not fit. With FW_TEXT_ELLIPSIS, trailing spaces before the ellipsis are
 * dropped. Returns the width actually drawn. */
int fw_text_draw_fit(const FwSurface *surface, int x, int y, int max_width, const char *utf8,
                     FwRgb color, int scale, FwOverflow overflow, bool *truncated);

/* Number of glyphs the text draws (each advances 6 x scale). */
int fw_text_glyph_count(const char *utf8);

/* Like fw_text_draw, but opaque: every glyph's whole 6x8 cell is written once,
 * unlit pixels in bg, as stock text is drawn (so redrawing never flickers). */
int fw_text_draw_bg(const FwSurface *surface, int x, int y, const char *utf8, FwRgb color, FwRgb bg, int scale);

/* Draws one row glyph at (x, y), scaled 1..4: lit pixels only, or with bg
 * non-NULL the whole 6x8 cell. */
void fw_text_draw_rows(const FwSurface *surface, int x, int y, const uint8_t rows[8], FwRgb color,
                       const FwRgb *bg, int scale);

/* Horizontal position that centres text of the given width in [left, left+span). */
int fw_text_center_x(int left, int span, int text_width);

#ifdef __cplusplus
}
#endif
#endif
