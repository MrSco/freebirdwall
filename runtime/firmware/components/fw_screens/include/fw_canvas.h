#ifndef FW_CANVAS_H
#define FW_CANVAS_H
/* An offscreen frame for animation: draw a whole frame into it (as RGB565,
 * the panel's own precision), then fw_canvas_flush() writes only the pixels
 * that differ from what the panel shows. The panel is single-buffered, so
 * clearing and redrawing it directly every frame would flicker. */
#include <stdbool.h>
#include <stdint.h>
#include "fw_text.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width, height;
    uint16_t *frame;     /* being drawn: width * height */
    uint16_t *shown;     /* what the panel shows */
    bool valid;          /* shown matches the panel */
} FwCanvas;

/* Buffers are the caller's: each width * height uint16_t. */
void fw_canvas_init(FwCanvas *c, int width, int height, uint16_t *frame, uint16_t *shown);

/* A surface that draws into the canvas frame. */
FwSurface fw_canvas_surface(FwCanvas *c);

/* The panel was drawn by someone else: the next flush writes every pixel. */
void fw_canvas_invalidate(FwCanvas *c);

/* Writes the changed pixels (all of them after an invalidate) to the panel,
 * in runs along each row through panel->fill when there is one. Returns the
 * number of pixels written. */
int fw_canvas_flush(FwCanvas *c, const FwSurface *panel);

uint16_t fw_rgb565(FwRgb c);
FwRgb fw_rgb888(uint16_t c);   /* top bits replicated, as the panel library does */

#ifdef __cplusplus
}
#endif
#endif
