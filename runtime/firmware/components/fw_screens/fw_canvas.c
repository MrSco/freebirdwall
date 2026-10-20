/* Offscreen frame with change-only flushing; see fw_canvas.h. */
#include "fw_canvas.h"
#include <string.h>

uint16_t fw_rgb565(FwRgb c){
    return (uint16_t)(((c.r & 0xf8) << 8) | ((c.g & 0xfc) << 3) | (c.b >> 3));
}

FwRgb fw_rgb888(uint16_t c){
    FwRgb o;
    o.r = (uint8_t)((c >> 8) & 0xf8);
    o.g = (uint8_t)((c >> 3) & 0xfc);
    o.b = (uint8_t)(c << 3);
    o.r = (uint8_t)(o.r | (o.r >> 5));
    o.g = (uint8_t)(o.g | (o.g >> 6));
    o.b = (uint8_t)(o.b | (o.b >> 5));
    return o;
}

void fw_canvas_init(FwCanvas *c, int width, int height, uint16_t *frame, uint16_t *shown){
    c->width = width;
    c->height = height;
    c->frame = frame;
    c->shown = shown;
    c->valid = false;
    memset(frame, 0, (size_t)width * (size_t)height * sizeof *frame);
}

static void put(void *ctx, int x, int y, FwRgb color){
    FwCanvas *c = ctx;
    c->frame[y * c->width + x] = fw_rgb565(color);
}

static void fill(void *ctx, int x, int y, int w, int h, FwRgb color){
    FwCanvas *c = ctx;
    uint16_t v = fw_rgb565(color);
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i) c->frame[j * c->width + i] = v;
}

FwSurface fw_canvas_surface(FwCanvas *c){
    FwSurface s = {c, c->width, c->height, put, fill};
    return s;
}

void fw_canvas_invalidate(FwCanvas *c){ c->valid = false; }

int fw_canvas_flush(FwCanvas *c, const FwSurface *panel){
    int written = 0;
    for (int y = 0; y < c->height; ++y) {
        const uint16_t *f = c->frame + y * c->width;
        uint16_t *s = c->shown + y * c->width;
        int x = 0;
        while (x < c->width) {
            int run;
            if (c->valid && f[x] == s[x]) { ++x; continue; }
            /* a run of changed pixels of one colour */
            run = 1;
            while (x + run < c->width && f[x + run] == f[x] && (!c->valid || f[x + run] != s[x + run])) ++run;
            if (panel->fill && run > 1) panel->fill(panel->ctx, x, y, run, 1, fw_rgb888(f[x]));
            else for (int i = 0; i < run; ++i) panel->pixel(panel->ctx, x + i, y, fw_rgb888(f[x]));
            for (int i = 0; i < run; ++i) s[x + i] = f[x];
            written += run;
            x += run;
        }
    }
    c->valid = true;
    return written;
}
