#ifndef FLIGHTWALL_VENDOR_CHARMAP_H
#define FLIGHTWALL_VENDOR_CHARMAP_H
/* Tables generated from the vendor image by tools/make_vendor_tables.py. */
#include "fw_text.h"

#define FW_VENDOR_CHARMAP_COUNT 134
#define FW_VENDOR_EXTRA_COUNT 63

typedef struct { uint16_t cp; uint8_t glyph; } FwCharMapEntry;

extern const FwCharMapEntry fw_vendor_charmap[FW_VENDOR_CHARMAP_COUNT];
extern const FwRowGlyph fw_vendor_extra_glyphs[FW_VENDOR_EXTRA_COUNT];

#endif
