#ifndef FLIGHTWALL_FW_WEATHER_H
#define FLIGHTWALL_FW_WEATHER_H
/* METAR weather "when the sky is quiet", as vendor firmware 4.2.4 shows it
 * (WEATHER.md; FUN_ names are in that image): the record the feed sends under
 * "weather", its field rules, the text formatters, the three mini layouts
 * (summary, simple, technical) and their drawing. The owned firmware also
 * fills the same record from aviationweather.gov on the community source
 * (main/weather.cpp), and draws its own icons when the wall's filesystem has
 * none of the vendor's (fw_weather_icon_draw).
 *
 * Portable C: the board, the desktop tests and the dashboard preview run
 * this same code. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_text.h"
#include "fw_clock.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { FW_WX_CAT_NONE, FW_WX_VFR, FW_WX_MVFR, FW_WX_IFR, FW_WX_LIFR };
enum {
    FW_WX_COND_NONE, FW_WX_CLEAR, FW_WX_PARTLY_CLOUDY, FW_WX_CLOUDY, FW_WX_OVERCAST,
    FW_WX_FOG, FW_WX_RAIN, FW_WX_SNOW, FW_WX_THUNDER,
};

/* The record (FUN_4200eb28 / FUN_4201085c). Absent or out-of-range numbers
 * leave their has_ flag false. */
typedef struct {
    char stn[6];            /* ICAO station, 3-4 chars, first a letter or digit; "" = no record */
    char name[25];          /* station name, printable ASCII ('?' for anything else) */
    char raw[257];          /* raw METAR, printable ASCII, cut at the last space past 256 */
    uint8_t cat;            /* FW_WX_VFR.. */
    uint8_t cond;           /* FW_WX_CLEAR.. */
    uint32_t obs;           /* observation time, unix seconds; 0 = none */
    uint32_t utc_at;        /* when utc_off becomes utc_next, unix seconds; 0 = none */
    int16_t utc_off, utc_next;   /* station UTC offset, minutes */
    uint16_t wdir;          /* degrees true */
    uint16_t wspd, wgst;    /* knots */
    uint16_t vis_m;         /* metres */
    uint16_t ceil_ft;       /* feet */
    int8_t temp_c;
    bool has_wdir, has_wspd, has_wgst, has_vis, has_ceil, has_temp, has_utc_off, has_utc_next;
    bool is_auto, night, wvar, vis_plus;
} FwWeather;

void fw_weather_init(FwWeather *w);
/* FUN_420e0980: a station and an observation time. */
bool fw_weather_valid(const FwWeather *w);

/* The record's keys with the vendor's value rules (FUN_4201085c); unknown keys
 * are ignored. Numbers that are not whole where whole numbers are needed, or
 * out of range, leave the field unset. */
void fw_weather_set_str(FwWeather *w, const char *key, const char *value);
void fw_weather_set_num(FwWeather *w, const char *key, double value);
void fw_weather_set_bool(FwWeather *w, const char *key, bool value);

/* ------------------------------------------------------------ owned: aviationweather.gov
 * On the community source (and when the FlightWall feed sends none) the wall
 * reads the METAR itself: GET https://aviationweather.gov/api/data/metar
 * ?ids=<station>&format=json (main/weather.cpp). These are the fields it
 * keeps, as read from the JSON; NAN / NULL = absent. */
#define FW_AWC_CLOUDS 6
typedef struct {
    const char *icao, *name, *raw, *flt_cat, *wx;   /* icaoId, name, rawOb, fltCat, wxString */
    double obs;            /* obsTime, unix seconds */
    double temp;           /* °C */
    double wdir;           /* degrees; NAN with wdir_vrb */
    bool wdir_vrb;         /* wdir "VRB" */
    double wspd, wgst;     /* knots */
    double visib;          /* statute miles; with visib_plus "10+" */
    bool visib_plus;
    double lat, lon;       /* the station, for day or night */
    int ncloud;
    const char *cover[FW_AWC_CLOUDS];   /* FEW SCT BKN OVC OVX CLR SKC CAVOK */
    double base[FW_AWC_CLOUDS];         /* feet */
} FwAwcMetar;

/* The record the vendor feed would have sent for this METAR: the category
 * as given, the condition from the weather and clouds (thunder, snow, rain,
 * fog, then overcast / cloudy / partly cloudy / clear), the ceiling (lowest
 * broken or overcast layer), visibility in metres, and night from the sun's
 * height at the station when observed. No station UTC offset (the clock's
 * zone is used). */
void fw_weather_from_awc(const FwAwcMetar *m, FwWeather *w);

/* The sun below the horizon (-0.833 degrees, refraction included) at lat/lon at t. */
bool fw_weather_is_night(double lat, double lon, int64_t t);

/* ------------------------------------------------------------ settings */

typedef enum { FW_WX_SUMMARY = 0, FW_WX_SIMPLE = 1, FW_WX_TECHNICAL = 2 } FwWeatherLayout;

typedef struct {
    uint8_t layout;         /* display_config.weather.layout */
    uint8_t temp_unit;      /* metric_display.temp_unit: 0 celsius, 1 fahrenheit */
    uint8_t wind_unit;      /* .wind_unit: 0 knots, 1 mph, 2 kmh, 3 mps */
    uint8_t vis_unit;       /* .vis_unit: 0 auto (SM for K/P stations, else km), 1 miles, 2 kilometers */
    uint8_t ceil_unit;      /* .ceil_unit: 0 feet, 1 meters */
    bool is_24h, show_seconds;   /* the clock's */
    FwZone zone;            /* the clock's zone, used when the record has no station offset */
} FwWeatherSettings;

FwWeatherLayout fw_weather_layout_parse(const char *s);   /* summary (default), simple, technical */
uint8_t fw_weather_temp_unit(const char *s);
uint8_t fw_weather_wind_unit(const char *s);
uint8_t fw_weather_vis_unit(const char *s);
uint8_t fw_weather_ceil_unit(const char *s);

/* ------------------------------------------------------------ text */

#define FW_WX_STALE_S 10800        /* older than 3 h: dropped, the clock again */
#define FW_WX_AGE_SHOWN_S 5400     /* older than 90 min: "1h35m ago" in orange */
#define FW_WX_PAGE_MS 4000         /* technical: a page of raw METAR every 4 s */

/* Seconds since the observation (FUN_4200ee88): -1 when the clock is not set,
 * 0 for an observation from the future. */
long fw_weather_age(const FwWeather *w, int64_t now);

/* Formatters (FUN_4200ebc0, FUN_42006d7c, FUN_42006f00, FUN_4200ed10). style
 * 0 long ("Wind 350° at 23 G36 kt"), 1 compact ("350°/23G36kt", "10SM",
 * "Ceil 7000ft", "9°C"), 2 summary value ("7,000 ft"). Return false (and "")
 * when the field is absent. */
bool fw_weather_wind(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap);
bool fw_weather_vis(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap);
bool fw_weather_ceil(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap);
bool fw_weather_temp(const FwWeather *w, const FwWeatherSettings *st, int style, char *out, size_t cap);
/* The local time line: the station's offset when the record has one, else the
 * clock's zone; 12/24 h and seconds as the clock. "" when the clock is not set. */
void fw_weather_time(const FwWeather *w, const FwWeatherSettings *st, int64_t now, char *out, size_t cap);
const char *fw_weather_cat_name(uint8_t cat);        /* VFR MVFR IFR LIFR, else "---" */
uint16_t fw_weather_cat_color(uint8_t cat);          /* RGB565 */
/* Icon name ("clear_day", "rain", ...) or NULL (FUN_4200ef48). */
const char *fw_weather_icon_name(const FwWeather *w);

/* ------------------------------------------------------------ screen */

#define FW_WX_SEGS 6
#define FW_WX_ROWS 5

typedef struct { char text[24]; uint16_t color; } FwWxSeg;
typedef struct {
    int16_t x, y, w;
    uint8_t max_size;      /* the text grows to this scale while it fits */
    bool center;
    uint8_t nseg;
    FwWxSeg seg[FW_WX_SEGS];
} FwWxRow;

/* One weather frame, built from the record (FUN_4200fe6c simple, FUN_4200fbec
 * summary, FUN_4200fffc technical). Two equal frames draw the same pixels, so
 * the display redraws only when the frame changed. */
typedef struct {
    uint8_t layout;
    char icon[24];         /* "" = none; drawn 32x32 at (4,6) */
    uint8_t nrow;
    FwWxRow row[FW_WX_ROWS];
} FwWxFrame;

/* page_ms: milliseconds since this raw METAR was first shown (technical pages). */
void fw_weather_frame(const FwWeather *w, const FwWeatherSettings *st, int64_t now, uint32_t page_ms, FwWxFrame *out);

/* The icon when the wall's filesystem has none: draws the owned 32x32 icon
 * for a name from fw_weather_icon_name at (x, y). */
void fw_weather_icon_draw(const FwSurface *s, int x, int y, const char *name);

/* Clears the panel and draws the frame. icon_drawn: the caller already put the
 * vendor's PNG at (4,6); otherwise the owned icon is drawn there. */
void fw_weather_draw(const FwSurface *s, const FwWxFrame *f, bool icon_drawn);

/* The rows only, opaque, for a redraw when only the text changed. With
 * hashes (FW_WX_ROWS entries, may be NULL), only rows whose hash changed are
 * drawn, and the hashes are updated. */
void fw_weather_draw_rows(const FwSurface *s, const FwWxFrame *f, uint32_t *hashes);
/* A hash of one row's position, size, texts and colours. */
uint32_t fw_weather_row_hash(const FwWxRow *r);

#ifdef __cplusplus
}
#endif
#endif
