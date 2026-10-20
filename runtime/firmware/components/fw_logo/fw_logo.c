#include "fw_logo.h"
#include <math.h>
#include <string.h>
#include "PNGdec.h"

/* Exported by PNGdec's C build (png.inl); the vendor's C++ getLineAsRGB565()
 * calls exactly this with hasAlpha() of the image. */
void PNGRGB565(PNGDRAW *pDraw, uint16_t *pPixels, int iEndiannes, uint32_t u32Bkgd, int iHasAlpha);

/* Vendor guard (FUN_4201b410): width x bits-per-channel x channel factor must
 * not exceed 3200; factor 3 for RGB, 2 for grey+alpha, 4 for RGBA, else 1. */
#define VENDOR_LINE_LIMIT 3200
#define MAX_LINE 400          /* the widest line that guard admits (8-bit palette) */

typedef struct {
    PNGIMAGE *png;
    FwLogo *out;
    int src_w, src_h;         /* PNG size */
    int dst_w, dst_h;         /* fitted size */
    int ox, oy;               /* fitted image's offset inside the square */
    bool scaled;
    int last_row;             /* destination row last written (scaled path) */
    uint16_t line[MAX_LINE];
} DecodeState;

static DecodeState s_state;   /* one decode at a time, as in the stock firmware */

static void plot(FwLogo *o, int x, int y, uint16_t c){
    int i;
    if (x < 0 || y < 0 || x >= o->area_w || y >= o->area_h) return;   /* clip to the square or box */
    x -= o->img_x;
    y -= o->img_y;
    if (x < 0 || y < 0 || x >= FW_LOGO_MAX_SIDE || y >= FW_LOGO_MAX_SIDE) return;
    i = y * FW_LOGO_MAX_SIDE + x;
    o->px[i] = c;
    o->mask[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* PNGdec line callback, mirroring the vendor's (0x4201b74c). */
static int on_line(PNGDRAW *d){
    DecodeState *st = (DecodeState *)d->pUser;
    int x;
    if (d->iWidth > MAX_LINE) return 0;
    PNGRGB565(d, st->line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000u, PNG_hasAlpha(st->png));
    if (!st->scaled) {
        for (x = 0; x < d->iWidth; ++x) plot(st->out, st->ox + x, st->oy + d->y, st->line[x]);
        return 1;
    }
    {
        /* destination row = (y*dstH + srcH/2) / srcH; only the first source row
         * landing on a destination row is drawn */
        long long num = (long long)d->y * st->dst_h + st->src_h / 2;
        int row = st->oy + (int)(num / st->src_h);
        if (row == st->last_row) return 1;
        st->last_row = row;
        for (x = 0; x < st->dst_w; ++x) {
            long long sn = (long long)x * d->iWidth + (st->dst_w >> 1);
            int sx = (int)(sn / st->dst_w);
            if (sx < 0) sx = 0;
            if (sx >= d->iWidth) sx = d->iWidth - 1;
            plot(st->out, st->ox + x, row, st->line[sx]);
        }
    }
    return 1;
}

int fw_logo_side_for_box(int box_w, int box_h){
    int side = 1;
    if (box_h - 4 > 0) {
        side = box_w - 4;
        if (side < 1) side = 1;
        if (side > box_h - 4) side = box_h - 4;
        if (side > FW_LOGO_MAX_SIDE) side = FW_LOGO_MAX_SIDE;
    }
    return side;
}

size_t fw_logo_decoder_bytes(void){ return sizeof(PNGIMAGE); }

/* FUN_4201b410 for an area of area_w x area_h; square = the airline path. */
static int decode(void *decoder, const uint8_t *png, size_t png_len, int area_w, int area_h, bool square,
                  FwLogo *out){
    PNGIMAGE *p = (PNGIMAGE *)decoder;
    DecodeState *st = &s_state;
    int w, h, bpp, type, line;
    float fw, fh, scale, sh;
    int rc;

    memset(out, 0, sizeof *out);
    out->side = square ? area_w : 0;
    out->area_w = area_w;
    out->area_h = area_h;
    memset(p, 0, sizeof *p);
    if (PNG_openRAM(p, (uint8_t *)png, (int)png_len, on_line) != PNG_SUCCESS) return FW_LOGO_ERR_OPEN;

    w = PNG_getWidth(p);
    h = PNG_getHeight(p);
    bpp = PNG_getBpp(p);
    type = PNG_getPixelType(p);
    if (w <= 0 || bpp <= 0 || h <= 0) { PNG_close(p); return FW_LOGO_ERR_OPEN; }
    line = w * bpp;
    if (type == PNG_PIXEL_TRUECOLOR) line *= 3;
    else if (type == PNG_PIXEL_GRAY_ALPHA) line *= 2;
    else if (type == PNG_PIXEL_TRUECOLOR_ALPHA) line *= 4;
    if (line > VENDOR_LINE_LIMIT || w > MAX_LINE) { PNG_close(p); return FW_LOGO_ERR_LIMIT; }

    /* Single-precision, as on the board: scale = min(area_w/w, area_h/h),
     * applied only when shrinking, sizes floored and at least 1. */
    fw = (float)w;
    fh = (float)h;
    scale = (float)area_w / fw;
    sh = (float)area_h / fh;
    if (sh < scale) scale = sh;
    if (scale <= 1.0f) { fw = fw * scale; fh = fh * scale; }
    st->dst_w = (int)floorf(fw);
    st->dst_h = (int)floorf(fh);
    if (st->dst_w < 1) st->dst_w = 1;
    if (st->dst_h < 1) st->dst_h = 1;

    st->png = p;
    st->out = out;
    st->src_w = w;
    st->src_h = h;
    st->scaled = (w != st->dst_w) || (h != st->dst_h);
    st->ox = (area_w - st->dst_w) / 2;
    st->oy = (area_h - st->dst_h) / 2;
    if (!square) {
        if (st->dst_w > FW_LOGO_MAX_SIDE || st->dst_h > FW_LOGO_MAX_SIDE) { PNG_close(p); return FW_LOGO_ERR_LIMIT; }
        out->img_x = st->ox;                    /* store the image itself, not the whole box */
        out->img_y = st->oy;
    }
    st->last_row = -1;
    out->drawn_w = st->dst_w;
    out->drawn_h = st->dst_h;

    rc = PNG_decode(p, st, 0);
    PNG_close(p);
    return rc == PNG_SUCCESS ? FW_LOGO_OK : FW_LOGO_ERR_DECODE;
}

int fw_logo_decode(void *decoder, const uint8_t *png, size_t png_len, int side, FwLogo *out){
    if (!decoder || !png || !png_len || png_len > 0x7fffffff || !out || side < 1 || side > FW_LOGO_MAX_SIDE)
        return FW_LOGO_ERR_ARGS;
    return decode(decoder, png, png_len, side, side, true, out);
}

int fw_logo_decode_box(void *decoder, const uint8_t *png, size_t png_len, int box_w, int box_h, FwLogo *out){
    if (!decoder || !png || !png_len || png_len > 0x7fffffff || !out || box_w < 1 || box_h < 1)
        return FW_LOGO_ERR_ARGS;
    return decode(decoder, png, png_len, box_w, box_h, false, out);
}

void fw_logo_draw_box(const FwSurface *s, int box_x, int box_y, const FwLogo *logo){
    if (!s || !s->pixel || !logo) return;
    for (int y = 0; y < FW_LOGO_MAX_SIDE; ++y)
        for (int x = 0; x < FW_LOGO_MAX_SIDE; ++x) {
            int i = y * FW_LOGO_MAX_SIDE + x, px = box_x + logo->img_x + x, py = box_y + logo->img_y + y;
            if (!(logo->mask[i >> 3] & (1u << (i & 7)))) continue;
            if (px < 0 || py < 0 || px >= s->width || py >= s->height) continue;
            s->pixel(s->ctx, px, py, fw_logo_rgb565_to_888(logo->px[i]));
        }
}

FwRgb fw_logo_rgb565_to_888(uint16_t c){
    FwRgb o;
    o.r = (uint8_t)((c >> 8) & 0xf8);
    o.g = (uint8_t)((c >> 3) & 0xfc);
    o.b = (uint8_t)(c << 3);
    o.r = (uint8_t)(o.r | (o.r >> 5));
    o.g = (uint8_t)(o.g | (o.g >> 6));
    o.b = (uint8_t)(o.b | (o.b >> 5));
    return o;
}

void fw_logo_draw(const FwSurface *s, int box_x, int box_y, int box_w, int box_h, const FwLogo *logo){
    int sq_x, sq_y, x, y;
    if (!s || !s->pixel || !logo || logo->side < 1) return;
    sq_x = box_x + (box_w - logo->side) / 2;
    sq_y = box_y + (box_h - logo->side) / 2;
    for (y = 0; y < logo->side; ++y)
        for (x = 0; x < logo->side; ++x) {
            int i = y * FW_LOGO_MAX_SIDE + x, px = sq_x + x, py = sq_y + y;
            if (!(logo->mask[i >> 3] & (1u << (i & 7)))) continue;
            if (px < 0 || py < 0 || px >= s->width || py >= s->height) continue;
            s->pixel(s->ctx, px, py, fw_logo_rgb565_to_888(logo->px[i]));
        }
}

static bool code_ok(const char *c){
    int i;
    if (!c || strlen(c) != 3) return false;
    for (i = 0; i < 3; ++i) {
        unsigned char ch = (unsigned char)c[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) return false;
    }
    return true;
}

const char *fw_logo_resolve(const char *al_code, const char *ac_class,
                            bool (*exists)(void *ctx, const char *path), void *ctx,
                            char *buf, size_t buf_len){
    static const struct { const char *cls, *path; } categories[] = {
        {"Heli", FW_LOGO_DIR "heli.png"},
        {"Light", FW_LOGO_DIR "light.png"},
        {"Smalljet", FW_LOGO_DIR "smalljet.png"},
    };
    size_t i;
    if (!exists || !buf || buf_len < FW_LOGO_PATH_MAX) return NULL;
    if (code_ok(al_code)) {
        /* Stock uppercases the code and requires three characters; it accepts
         * any bytes, but every shipped logo name is three letters, so limiting
         * to letters and digits changes no result and keeps paths tame. */
        memcpy(buf, FW_LOGO_DIR, sizeof FW_LOGO_DIR - 1);
        for (i = 0; i < 3; ++i) {
            char ch = al_code[i];
            buf[sizeof FW_LOGO_DIR - 1 + i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        }
        memcpy(buf + sizeof FW_LOGO_DIR - 1 + 3, ".png", 5);
        if (exists(ctx, buf)) return buf;
    }
    if (ac_class)
        for (i = 0; i < sizeof categories / sizeof categories[0]; ++i)
            if (strcmp(ac_class, categories[i].cls) == 0) {
                if (exists(ctx, categories[i].path)) return categories[i].path;
                break;
            }
    return exists(ctx, FW_LOGO_DEFAULT_PATH) ? FW_LOGO_DEFAULT_PATH : NULL;
}
