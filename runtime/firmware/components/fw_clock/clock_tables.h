#ifndef FLIGHTWALL_CLOCK_TABLES_H
#define FLIGHTWALL_CLOCK_TABLES_H
/* Tables generated from the vendor image by tools/make_vendor_tables.py. */
#include <stdint.h>
#include "fw_text.h"

#define FW_CLOCK_DATE_GLYPHS 42
#define FW_CLOCK_LOCALES 19
#define FW_CLOCK_ZONE_NAMES 20

typedef struct {
    const char *code;
    const char *weekdays[7];   /* Sunday first */
    const char *months[12];
} FwClockLocale;

typedef struct { int16_t minutes; const char *name; } FwZoneName;

extern const FwRowGlyph fw_clock_date_glyphs[FW_CLOCK_DATE_GLYPHS];
extern const FwClockLocale fw_clock_locales[FW_CLOCK_LOCALES];
extern const FwZoneName fw_clock_zone_names[FW_CLOCK_ZONE_NAMES];

#endif
