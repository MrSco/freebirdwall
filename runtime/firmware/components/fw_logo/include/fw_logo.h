#ifndef FLIGHTWALL_FW_LOGO_H
#define FLIGHTWALL_FW_LOGO_H
/* Airline logos, drawn exactly as the stock firmware draws them.
 *
 * Every rule here was recovered from the vendor application (0ab195d6; see
 * ../../LOGOS.md for addresses):
 *  - lookup: three-character al_code, uppercased, ->
 *    /images/airlines/square/<CODE>.png; else the aircraft class Heli / Light
 *    / Smalljet picks a category image; else default.png;
 *  - an image element box (w x h) holds a square of side
 *    clamp(min(w-4, h-4), 1, 64), centred in the box;
 *  - PNGdec decodes each line to RGB565, alpha blended over black;
 *  - the image is fitted into the square with single-precision float scaling
 *    (never enlarged), centred, then sampled nearest-neighbour with the
 *    vendor's integer rounding (first source row wins), clipped to the square;
 *  - pixels reach the panel through RGB565 -> RGB888 bit replication.
 *
 * Decoding is separate from drawing: decode once per airline into an FwLogo,
 * draw it every frame. Plain C, no SDK dependency, so desktop tests and the
 * preview run the same code. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_text.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_LOGO_MAX_SIDE 64
#define FW_LOGO_DIR "/images/airlines/square/"
#define FW_LOGO_DEFAULT_PATH FW_LOGO_DIR "default.png"
#define FW_LOGO_PATH_MAX 48

/* A decoded logo: what the stock firmware would have drawn into its square,
 * relative to the square's top-left. Only pixels with their mask bit set were
 * written (the rest of the square is left as it was). RGB565 little endian. */
typedef struct {
    int side;
    int drawn_w, drawn_h;                       /* fitted image size, for reports */
    int area_w, area_h;                         /* the square (side x side) or the box it was fitted to */
    int img_x, img_y;                           /* where px[0] sits in that area (0,0 for a square) */
    uint16_t px[FW_LOGO_MAX_SIDE * FW_LOGO_MAX_SIDE];
    uint8_t mask[FW_LOGO_MAX_SIDE * FW_LOGO_MAX_SIDE / 8];
} FwLogo;

enum {
    FW_LOGO_OK = 0,
    FW_LOGO_ERR_ARGS = -1,
    FW_LOGO_ERR_OPEN = -2,      /* not a PNG PNGdec accepts */
    FW_LOGO_ERR_LIMIT = -3,     /* the stock firmware's line-size limit refuses it */
    FW_LOGO_ERR_DECODE = -4,
};

/* Side of the square the stock firmware draws for an image element box. */
int fw_logo_side_for_box(int box_w, int box_h);

/* Bytes of working memory the decoder needs (PNGdec state, ~43 KiB). Allocate
 * it only around fw_logo_decode() and free it afterwards. */
size_t fw_logo_decoder_bytes(void);

/* Decodes a PNG into `out` exactly as the stock firmware would draw it into a
 * square of `side`. `decoder` must be fw_logo_decoder_bytes() bytes, suitably
 * aligned. Not reentrant (one decode at a time). */
int fw_logo_decode(void *decoder, const uint8_t *png, size_t png_len, int side, FwLogo *out);

/* Decodes a PNG fitted to a whole w x h box rather than a square, as stock
 * draws other images (FUN_4201af7c with centring, e.g. the boot splash logo):
 * the same scaling, sampling and colours, centred in the box and clipped to
 * it. The fitted image must be at most FW_LOGO_MAX_SIDE square
 * (FW_LOGO_ERR_LIMIT otherwise). */
int fw_logo_decode_box(void *decoder, const uint8_t *png, size_t png_len, int box_w, int box_h, FwLogo *out);

/* Draws a logo from fw_logo_decode_box with its box at (box_x, box_y). */
void fw_logo_draw_box(const FwSurface *s, int box_x, int box_y, const FwLogo *logo);

/* Draws a decoded logo for an image element box: square centred in the box. */
void fw_logo_draw(const FwSurface *s, int box_x, int box_y, int box_w, int box_h, const FwLogo *logo);

/* The panel library's RGB565 -> RGB888 expansion (top bits replicated). */
FwRgb fw_logo_rgb565_to_888(uint16_t c);

/* Lookup, as the stock resolver: returns the path to use, or NULL when even
 * default.png is missing (nothing is drawn). `exists` reports whether a path
 * is present on the filesystem. `buf` must hold FW_LOGO_PATH_MAX bytes. */
const char *fw_logo_resolve(const char *al_code, const char *ac_class,
                            bool (*exists)(void *ctx, const char *path), void *ctx,
                            char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
#endif
