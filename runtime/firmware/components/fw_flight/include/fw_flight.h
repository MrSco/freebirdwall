#ifndef FLIGHTWALL_FW_FLIGHT_H
#define FLIGHTWALL_FW_FLIGHT_H
/* Flight text exactly as the stock firmware builds it: the flight record, the
 * {token} template engine with its unit formatting, the built-in element
 * templates, and the tracked-flight states, pages and footers. Rules and the
 * vendor functions they come from are in ../../FLIGHT-SCREENS.md.
 * Portable C (no drawing): desktop tests and previews run this same code. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One feed row (FUN_42010604). Strings are UTF-8, empty when absent; numbers
 * are NAN when absent. Sizes follow the vendor record. */
typedef struct {
    char icao[8], cs[12], reg[16];          /* reg: at most 15 after clean-up */
    char fn[8];              /* 4.2: IATA flight number ("NZ7"), for flight_identity; longer ones are dropped */
    double alt_b, alt_g, vel, trk, vr;   /* metres, m/s, degrees, m/s */
    bool gnd;
    char sqk[8], orig_icao[8], orig_iata[8], dest_icao[8], dest_iata[8];
    char orig[32], dest[32];
    char al_code[8], al_paint[8], al_op[8], al_name[24];   /* 4.2 sends up to 35 bytes: a longer name is cut (al_name_cut) and al_short used */
    char al_short[16];       /* 4.2: the airline's short name ("Air NZ"), used when al_name does not fit */
    bool al_name_cut, al_short_cut;   /* 4.2: cut to fit the record (FUN_42008770) */
    char ac_type[16], ac_name[32], ac_class[16];   /* ac_type: the ICAO type designator ("B789"); 4.2's
                                                     * ac_icao fills it when the feed leaves it empty */
    /* trk_info: tracked flights only */
    bool has_trk;
    char sts;                /* 'C' cancelled, 'D' delayed, 0 none */
    int64_t sexp;            /* when the status text stops (unix seconds) */
    double prog, dist;       /* percent, metres */
    char conf[16], dep[24], arr[24], over[32], tft[24];
    bool trk_gnd;
    bool trk_metrics;        /* owned: the metrics page for this tracked flight
                              * (its show_metrics setting; the vendor feed
                              * leaves it off, as stock) */
} FwFlight;

/* Fills every number with NAN and every string with "". */
void fw_flight_init(FwFlight *f);

typedef enum { FW_ALT_METERS, FW_ALT_FEET, FW_ALT_KILOMETERS, FW_ALT_NAUTICAL_MILES } FwAltUnit;
typedef enum { FW_VEL_KMH, FW_VEL_MPS, FW_VEL_KNOTS, FW_VEL_MPH, FW_VEL_MACH } FwVelUnit;
typedef enum { FW_VR_KMH, FW_VR_MPS, FW_VR_FPS, FW_VR_FPM, FW_VR_MPH } FwVrUnit;
typedef enum { FW_DIST_METERS, FW_DIST_KILOMETERS, FW_DIST_MILES, FW_DIST_NAUTICAL_MILES, FW_DIST_FEET } FwDistUnit;
typedef enum { FW_TITLE_AIRLINE, FW_TITLE_CALLSIGN, FW_TITLE_REGISTRATION } FwTitle;

typedef struct {
    uint8_t alt, vel, vr, dist;   /* the enums above */
    bool trk_cardinal;            /* trk_unit "cardinal", else degrees */
} FwUnits;

/* Stock metric_display strings -> enums (FUN_420081e8); unknown/NULL -> the
 * stock default (meters, kmh, kmh, degrees, meters). */
void fw_units_parse(FwUnits *u, const char *alt_b_unit, const char *vel_unit, const char *vr_unit,
                    const char *trk_unit, const char *dist_unit);
/* Stock title_text ("registration", "callsign", anything else = airline). */
FwTitle fw_title_parse(const char *title_text);

/* ------------------------------------------------------------ 4.2 slots (LAYOUTS.md)
 * "Customize slots": each layout's lines (or halves of lines) as slots that
 * can show one field. FUN_4200745c (slots), table 0x3c0f5e34 (field names)
 * and 0x3c0f367c (their templates). */
#define FW_SLOTS_MAX 9
enum {
    FW_FIELD_EMPTY, FW_FIELD_AIRLINE, FW_FIELD_CALLSIGN, FW_FIELD_REGISTRATION, FW_FIELD_AIRCRAFT_TYPE,
    FW_FIELD_ORIGIN, FW_FIELD_DESTINATION, FW_FIELD_ROUTE, FW_FIELD_ALTITUDE, FW_FIELD_SPEED, FW_FIELD_TRACK,
    FW_FIELD_VERTICAL_RATE, FW_FIELD_COUNT
};
typedef struct {
    const char *name;     /* "heading2a" */
    const char *parent;   /* the layout element it sits in */
    int8_t half;          /* -1 the whole line, 0 left half, 1 right half */
    uint8_t dflt;         /* FW_FIELD_* shown when not overridden */
} FwSlot;

/* The slots of a layout (mini DEFAULT 5, NO_LOGO 7, TECHNICAL 7; NULL, 0 for others). */
const FwSlot *fw_flight_slots(bool mini, const char *layout, int *count);
int fw_flight_field_parse(const char *name);          /* "altitude" -> FW_FIELD_ALTITUDE; -1 unknown */
const char *fw_flight_field_name(int field);
const char *fw_flight_field_template(int field);      /* "{alt_b}"; "" for empty */

typedef struct {
    FwUnits units;
    FwTitle title;
    const char *const *anchors;   /* anchor_airports (ICAO or IATA), may be NULL */
    int anchor_count;
    bool mini;                    /* model mini-v1, else widescreen-v1 */
    const char *layout;           /* layout_template, e.g. "DEFAULT" */
    /* Owned addition, not in the vendor firmware: airport aliases. Asked for
     * {orig}, {dest}, {orig_iata} and {dest_iata} with the text about to be
     * substituted; returns true and fills out (cap bytes) to use an alias
     * instead. NULL = no aliases. */
    bool (*alias)(void *ctx, const char *token, const char *value, char *out, size_t cap);
    void *alias_ctx;
    /* Owned (owned_config.progress_bar, FLIGHT-SCREENS.md "Progress bar"):
     * the ordinary card draws the tracked card's bar along the bottom when
     * the flight's prog is known. */
    bool progress;
    /* 4.2 (LAYOUTS.md): display_config.flight_identity "iata_flight_number"
     * makes {cs} the IATA flight number (fn) when the feed has one. */
    bool flight_number;
    /* 4.2: metric_display.*_format: 0 stock, 1 "short" (no label; track 225°,
     * vertical rate ↑1200), 2 "flight_level" (altitude, with feet: FL350). */
    uint8_t alt_format, vel_format, vr_format, trk_format;
    /* 4.2: layout_overrides.area.<layout>.slots, per slot of fw_flight_slots():
     * 0 = the slot's default (so zeroed settings have no overrides), else the
     * field (FW_FIELD_*) + 1. */
    uint8_t slots[FW_SLOTS_MAX];
} FwFlightSettings;

#define FW_FLIGHT_TEXT_MAX 128

/* FUN_42008680: the punctuation clean-up applied to feed text. */
void fw_flight_clean(const char *in, char *out, size_t cap);

/* FUN_42008c18: expands every {token} in tmpl. NULL tmpl -> "". */
void fw_flight_expand(const FwFlightSettings *s, const FwFlight *f, const char *tmpl, char *out, size_t cap);

/* Owned (FLIGHT-SCREENS.md "Long text"): the card's elements are expanded as
 * stock, and only when a line is wider than its box again with shorter
 * names. fw_flight_text and fw_track_text (not the tracked footers) do this;
 * these are the rules. */
void fw_flight_short_airline(char *s, size_t cap);              /* "JetBlue Airways" -> "JetBlue" */
void fw_flight_short_airport(char *s, size_t cap, int level);   /* 1: Intl, Rgnl ...; 2: drop a last Intl */

/* FUN_42006f50: the built-in template for an element of a layout, or NULL. */
const char *fw_flight_element_template(bool mini, const char *layout, const char *element);

/* 4.2 (FUN_4200b2fc): the text drawn in a layout element with the owner's
 * slots: one part (the whole line) or two halves (left, right) when a half of
 * it is overridden. x and w are the element's; each part gets its own box.
 * fit: the layout has an override, so a line too long first loses its
 * Alt:/Spd:/Trk:/Vr: labels (the owned firmware then scrolls what is still
 * too long, where stock cut it with "..."). Returns 0 when the element shows
 * nothing. */
typedef struct {
    const char *name;    /* the element, or the half's slot name */
    char tmpl[48];
    int x, w;
    bool fit;
} FwCardPart;
int fw_flight_card_parts(const FwFlightSettings *s, const char *element, int x, int w, FwCardPart out[2]);

/* The text of one card part (heading1 rules, al_short fitting, label
 * stripping in fit mode). */
void fw_flight_part_text(const FwFlightSettings *s, const FwFlight *f, const FwCardPart *p, char *out, size_t cap);

/* ------------------------------------------------------------ tracked */

typedef struct {
    bool landed;      /* prog >= 100 */
    bool on_ground;   /* prog == 0 and the flight is on the ground */
    bool took;        /* landed with a flight time to show */
} FwTrackState;

void fw_track_state(const FwFlight *f, FwTrackState *t);

#define FW_TRACK_MAX_PAGES 5

/* FUN_4200e5f4: the pages a tracked flight cycles through (0 departed/arriving,
 * 1 flying over, 2 metrics, 3 distance). Returns the count. */
int fw_track_pages(const FwFlight *f, bool show_metrics, uint8_t pages[FW_TRACK_MAX_PAGES]);

/* Milliseconds each page stays up. */
uint32_t fw_track_page_ms(int page_count, int tracked_flights);

/* FUN_4200bc94/FUN_4200b9f4: the template for an element of the tracked card
 * (NULL = element left blank). */
const char *fw_track_template(const FwFlightSettings *s, const FwTrackState *t, int page, const char *element);

/* The element's final text, with every tracked-card rule applied:
 * CANCELLED/DELAYED, "Arriving now", mini footer shortening. now = unix
 * seconds. Returns false when the element is blank. */
bool fw_track_text(const FwFlightSettings *s, const FwFlight *f, const FwTrackState *t, int page,
                   const char *element, int element_w, int64_t now, char *out, size_t cap);

/* The ordinary card's text for an element (layout template, heading1 fit,
 * widescreen shortening). Returns false when the layout has no template. */
bool fw_flight_text(const FwFlightSettings *s, const FwFlight *f, const char *element, int element_w,
                    char *out, size_t cap);

/* ------------------------------------------------------------ several flights */

/* FUN_4200f3e4/FUN_4200e114/FUN_4200f048: one feed response holds at most
 * five flights plus a count of further flights; the display rotates through
 * them at flight_display_speed. */
#define FW_FEED_MAX_FLIGHTS 5

typedef enum { FW_MAXF_OFF, FW_MAXF_TEXT, FW_MAXF_INDICATOR } FwMaxFlights;

/* max_flights_behavior "off"/"text"/"indicator_led"; when absent or invalid,
 * hide_max_flights_screen decides (absent too -> text). */
FwMaxFlights fw_max_flights_parse(const char *behavior, bool hide_given, bool hide);

/* flight_display_speed: "slow" 5000, "fast" 2000, anything else 3000 ms. */
uint32_t fw_display_speed_ms(const char *speed);

typedef struct {
    FwFlight list[FW_FEED_MAX_FLIGHTS];
    int count;               /* flights in the last response */
    int overflow;            /* further flights the response reported */
    bool have_list;
    FwMaxFlights behavior;
    uint32_t dwell_ms;
    int cap;                 /* 4 while the "too many" screen takes a slot, else all */
    bool too_many;           /* the extra slot is in the rotation */
    bool on_too_many;        /* the extra slot is showing */
    bool indicator;          /* indicator_led overflow mark */
    int index;               /* slot shown; == shown flights means the "too many" screen */
    uint32_t since;          /* when the slot started (0 = not yet timed) */
    uint32_t extra_ms;       /* owned: this slot stays longer (a long line scrolling through once); 0 on each advance */
    bool dirty;
    bool have_current;
    char cur_icao[8], cur_cs[12];
} FwRotation;

void fw_rotation_init(FwRotation *r, FwMaxFlights behavior, uint32_t dwell_ms);

/* A new feed response (not tracked mode). */
void fw_rotation_set_list(FwRotation *r, const FwFlight *flights, int count, int overflow);

typedef enum {
    FW_SHOW_NOTHING_YET,   /* no response yet: the loading template with no text */
    FW_SHOW_NO_FLIGHTS,    /* empty response: the loading template, text "No flights..." */
    FW_SHOW_FLIGHT,
    FW_SHOW_TOO_MANY,
} FwShow;

/* Advances the rotation to now_ms. *redraw is set when the screen must be
 * drawn again; *flight points at the flight to show (NULL with
 * FW_SHOW_FLIGHT: keep the current picture). *more is the number for
 * "Seeing %d flights". */
FwShow fw_rotation_tick(FwRotation *r, uint32_t now_ms, const FwFlight **flight, int *more, bool *redraw);

/* Owned: the flight the rotation will show after the current slot (the
 * "too many" screen is skipped), so its logo can be decoded ahead; NULL when
 * there is no list or no flights. With one flight it is that flight. */
const FwFlight *fw_rotation_next(const FwRotation *r);

#ifdef __cplusplus
}
#endif
#endif
