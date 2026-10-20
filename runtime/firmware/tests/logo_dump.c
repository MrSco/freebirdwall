/* Decodes PNG files through fw_logo + PNGdec and writes what would be drawn,
 * for comparison with the independent Python reference (tests/test_logos.py).
 *   logo_dump <side | WxH> <out.bin> <list.txt>   (one PNG path per line)
 * A side decodes into a square (fw_logo_decode), WxH fits the whole box
 * (fw_logo_decode_box).
 * Record: name_len u16, name, rc i32, area_w u16, area_h u16, drawn_w u16,
 *         drawn_h u16, then area_w*area_h x (written u8, rgb565 u16). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_logo.h"

static FwLogo logo;

int main(int argc, char **argv){
    void *decoder;
    FILE *out, *list;
    char path[1024];
    int side, box_w = 0, box_h = 0, aw, ah;
    if (argc != 4) return 2;
    side = atoi(argv[1]);
    if (strchr(argv[1], 'x')) {
        box_w = side;
        box_h = atoi(strchr(argv[1], 'x') + 1);
    }
    aw = box_w ? box_w : side;
    ah = box_w ? box_h : side;
    decoder = malloc(fw_logo_decoder_bytes());
    out = fopen(argv[2], "wb");
    list = fopen(argv[3], "r");
    if (!decoder || !out || !list) return 2;
    while (fgets(path, sizeof path, list)) {
        FILE *f;
        unsigned char *buf;
        long n;
        int rc, x, y;
        const char *name;
        unsigned short nl, v;
        path[strcspn(path, "\r\n")] = 0;
        if (!*path) continue;
        f = fopen(path, "rb");
        if (!f) return 2;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)n);
        if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
        rc = box_w ? fw_logo_decode_box(decoder, buf, (size_t)n, box_w, box_h, &logo)
                   : fw_logo_decode(decoder, buf, (size_t)n, side, &logo);
        free(buf);
        name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        nl = (unsigned short)strlen(name);
        fwrite(&nl, 2, 1, out); fwrite(name, 1, nl, out); fwrite(&rc, 4, 1, out);
        v = (unsigned short)aw; fwrite(&v, 2, 1, out);
        v = (unsigned short)ah; fwrite(&v, 2, 1, out);
        v = (unsigned short)logo.drawn_w; fwrite(&v, 2, 1, out);
        v = (unsigned short)logo.drawn_h; fwrite(&v, 2, 1, out);
        for (y = 0; y < ah; ++y)
            for (x = 0; x < aw; ++x) {
                int sx = x - logo.img_x, sy = y - logo.img_y, k = sy * FW_LOGO_MAX_SIDE + sx;
                unsigned char w = 0;
                unsigned short c = 0;
                if (rc == 0 && sx >= 0 && sy >= 0 && sx < FW_LOGO_MAX_SIDE && sy < FW_LOGO_MAX_SIDE) {
                    w = (logo.mask[k >> 3] >> (k & 7)) & 1;
                    c = logo.px[k];
                }
                fwrite(&w, 1, 1, out); fwrite(&c, 2, 1, out);
            }
    }
    fclose(list);
    fclose(out);
    free(decoder);
    return 0;
}
