#ifndef FLIGHTWALL_FW_CLOCK_H
#define FLIGHTWALL_FW_CLOCK_H
/* The stock clock screen (the CLOCK loading template), rebuilt from the vendor
 * firmware; see ../../CLOCK.md for every rule and where it came from.
 * Portable C: desktop tests and previews run this same code. */
#include <stdbool.h>
#include <stdint.h>
#include "fw_text.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FW_DST_NONE = 0, FW_DST_NORTH_AMERICA = 1, FW_DST_EUROPE = 2 } FwDstRule;

#define FW_ZONE_UNSET INT16_MIN   /* stock "no zone" (0x8000): UTC, no DST, named "UTC" */

typedef struct {
    int16_t standard_offset_min;  /* e.g. -300 for US Eastern; FW_ZONE_UNSET for none */
    uint8_t dst_rule;             /* FwDstRule */
} FwZone;

typedef struct {
    char label[16];   /* shown before its time; empty = the zone's stock name */
    FwZone zone;
} FwExtraZone;

#define FW_CLOCK_MAX_EXTRA_ZONES 2   /* stock keeps at most two */
#define FW_CLOCK_MAX_LINES 3

typedef struct {
    FwZone zone;
    bool large;          /* clock.style "LARGE" (else "DEFAULT") */
    bool show_date;      /* clock.show_date */
    bool is_24h;         /* clock_is_24hr */
    bool show_seconds;   /* clock_show_seconds */
    bool day_first;      /* date_format.order "day_month" (else "month_day") */
    bool show_weekday;   /* date_format.show_weekday */
    const char *locale;  /* "en", "de", ...; unknown or NULL = English */
    int extra_count;     /* clock.additional_timezones */
    FwExtraZone extra[FW_CLOCK_MAX_EXTRA_ZONES];
} FwClockConfig;

/* Broken-down local time (fields as in struct tm: year since 1900, month 0-11,
 * weekday 0 = Sunday). */
typedef struct { int sec, min, hour, mday, mon, year, wday, yday; bool dst; } FwTime;

/* Local time for a zone exactly as stock computes it, DST rules included. */
bool fw_local_time(int64_t utc_seconds, FwZone zone, FwTime *out);

/* Stock name for a zone: its abbreviation (EST, CET...) or "UTC+5:30". */
void fw_zone_name(FwZone zone, char *buf, int cap);

/* Line text as stock formats it. buf >= 32 bytes. */
void fw_clock_time_text(const FwClockConfig *c, const FwTime *t, char *buf, int cap);
void fw_clock_date_text(const FwClockConfig *c, const FwTime *t, char *buf, int cap);

typedef enum { FW_CLOCK_DATE = 0, FW_CLOCK_TIME = 1, FW_CLOCK_ZONE = 2 } FwClockLineKind;

typedef struct {
    uint8_t kind;      /* FwClockLineKind */
    char text[40];
    int x, y, w, h;    /* w = glyphs x 6 x scale, h = 8 x scale */
} FwClockLine;

typedef struct {
    int count, scale;
    FwClockLine lines[FW_CLOCK_MAX_LINES];
} FwClockFrame;

/* Builds and lays out the lines for one moment, as stock FUN_42013874 and
 * FUN_42013b88 do: date (if shown), the time, then extra zones; centred as a
 * block, doubled in LARGE style when everything fits. */
void fw_clock_frame(const FwClockConfig *c, int64_t utc_seconds, int width, int height, FwClockFrame *out);

/* Draws a frame the way stock does every refresh: clears the panel only when
 * the layout changed from *prev (or prev is NULL / has no lines), otherwise
 * redraws just the lines whose text changed, opaquely so nothing flickers.
 * All text is white. *prev is updated to the frame drawn. */
void fw_clock_draw(const FwSurface *s, const FwClockFrame *frame, FwClockFrame *prev);

/* Convenience for previews: clear and draw the clock for one moment. */
void fw_screen_clock(const FwSurface *s, const FwClockConfig *c, int64_t utc_seconds);

#ifdef __cplusplus
}
#endif
#endif
