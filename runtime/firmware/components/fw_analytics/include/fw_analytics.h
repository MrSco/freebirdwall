#ifndef FW_ANALYTICS_H
#define FW_ANALYTICS_H
/* Bounded on-device flight analytics (ANALYTICS.md).
 *
 * Every flight the feed reports is recorded before the display picks its
 * five, deduplicated into sightings (a flight seen again within
 * FW_AN_REVISIT_S is the same sighting). Kept:
 *  - sightings per local hour (48 h) and per local day (31 d), with minutes
 *    actually observed so sleep and outages show as gaps;
 *  - top airlines, aircraft types, departure and arrival airports per day
 *    (space-saving), plus how many sightings had a known route;
 *  - a ring of recent rare aircraft sightings (curated global rarity tiers);
 *  - a 40x40 flight-path density grid (heatmap) over the primary watch area,
 *    interpolating segments between consecutive position fixes of a flight.
 *
 * Fixed size, no allocation; serialises to a versioned CRC-32 checkpoint for
 * optional LAN/NAS or browser backup, and streams JSON without heap.
 * Portable C. Times are unix seconds plus the local UTC offset in minutes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_AN_REVISIT_S 1800         /* unseen this long -> a new sighting */
#define FW_AN_RECENT 512             /* flights remembered for deduplication */
#define FW_AN_HOURS 48
#define FW_AN_DAYS 31
#define FW_AN_TOP 12                 /* airlines tracked per day (space-saving) */
#define FW_AN_DIM_TOP 6              /* types, origins and destinations per day */
#define FW_AN_CODE 4                 /* airline code, 3 characters + NUL */
#define FW_AN_RARE 16                /* recent rare aircraft sightings kept */
#define FW_AN_GRID 40                /* heatmap grid side (40x40 cells) */
#define FW_AN_TRACKS 32              /* active aircraft tracks for path segments */

enum {
    FW_AN_RARE_NONE = 0,
    FW_AN_RARE_NOTABLE = 1,
    FW_AN_RARE_VERY = 2,
    FW_AN_RARE_ONE_OF_KIND = 3,
};

typedef struct {
    char code[FW_AN_CODE];
    uint16_t count;                  /* may overcount by at most `error` */
    uint16_t error;
} FwAnAirline;

/* 1..4 uppercase ASCII characters, NUL-padded when shorter (8 bytes packed). */
typedef struct {
    char code[4];
    uint16_t count;
    uint16_t error;
} FwAnTag;

typedef struct {
    int32_t hour;                    /* local hours since the epoch; -1 = empty slot */
    uint16_t sightings;
    uint8_t minutes_observed;        /* 0..60 */
    uint8_t pad;
    uint64_t minute_bits;            /* which minutes were observed (60 used) */
} FwAnHour;

typedef struct {
    int32_t day;                     /* local days since the epoch; -1 = empty slot */
    uint16_t sightings;
    uint16_t minutes_observed;       /* 0..1440 */
    uint16_t routes_known;           /* sightings with an origin or destination */
    uint16_t pad;
    FwAnAirline top[FW_AN_TOP];      /* top airlines (3-letter ICAO) */
    FwAnTag types[FW_AN_DIM_TOP];    /* top ICAO aircraft types */
    FwAnTag orig[FW_AN_DIM_TOP];     /* top departure airports */
    FwAnTag dest[FW_AN_DIM_TOP];     /* top arrival airports */
} FwAnDay;

typedef struct {
    uint32_t key;                    /* flight identity hash, 0 = empty */
    uint32_t last_seen;
} FwAnRecent;

typedef struct {
    uint32_t seen_at;                /* unix seconds */
    uint32_t key;                    /* flight identity, for async route backfill */
    uint8_t tier;                    /* FW_AN_RARE_* (1..3) */
    char ac_type[5];
    char cs[10];
    char reg[10];
    char al[4];
    char orig[5];
    char dest[5];
} FwAnRare;

typedef struct {
    uint32_t key;
    uint32_t at;
    int8_t x, y;
    uint8_t used, pad;
} FwAnTrack;

typedef struct {
    float lat, lon;                  /* bounding box center */
    float dlat, dlon;                /* half-span in degrees; <= 0 = unset */
    uint32_t since;                  /* first position timestamp, 0 = none */
    uint16_t scale;                  /* count multiplier (>= 1) */
    uint16_t points;                 /* plotted position updates (clamped) */
    uint8_t cells[FW_AN_GRID * FW_AN_GRID];
} FwAnHeat;

typedef struct {
    FwAnHour hours[FW_AN_HOURS];     /* indexed by hour % FW_AN_HOURS */
    FwAnDay days[FW_AN_DAYS];        /* indexed by day % FW_AN_DAYS */
    FwAnRecent recent[FW_AN_RECENT];
    uint8_t route_bits[FW_AN_RECENT / 8];
    FwAnRare rare[FW_AN_RARE];
    uint8_t rare_head;
    uint8_t rare_count;
    uint16_t rare_total;
    FwAnHeat heat;
    FwAnTrack tracks[FW_AN_TRACKS];
    uint32_t total_sightings;
    uint32_t total_observations;
} FwAnalytics;

typedef struct {
    uint32_t key;
    const char *airline;             /* 3-letter ICAO or NULL */
    const char *ac_type;             /* 2..4 char ICAO type ("B738", "A21N") or NULL */
    const char *orig;                /* 3..4 char airport code (IATA/ICAO) or NULL */
    const char *dest;                /* 3..4 char airport code (IATA/ICAO) or NULL */
    const char *callsign;            /* optional, kept for rare sightings */
    const char *reg;                 /* optional, kept for rare sightings */
    bool has_pos;
    double lat, lon;
} FwAnObs;

void fw_an_init(FwAnalytics *a);

/* Identity of a flight for deduplication: the ICAO hex address when known,
 * else the callsign, else the registration (FNV-1a, never 0). */
uint32_t fw_an_flight_key(const char *hex, const char *callsign, const char *reg);

/* Global rarity tier for an ICAO aircraft type designator (case-insensitive):
 * FW_AN_RARE_NONE (0) .. FW_AN_RARE_ONE_OF_KIND (3). */
uint8_t fw_an_rarity(const char *ac_type);
const char *fw_an_rarity_label(uint8_t tier);

/* Sets the watch area(s) for the flight-path heatmap: a single circle or an
 * enclosing lat/lon box covering multiple areas. If the box changes
 * substantially (> 20%), the grid resets; otherwise it stays. */
void fw_an_set_area(FwAnalytics *a, double lat, double lon, double radius_km);
void fw_an_set_bounds(FwAnalytics *a, double lat_min, double lat_max, double lon_min, double lon_max);

/* One flight seen in a feed response. Returns true when it starts a new
 * sighting (and was counted). Positions are plotted on every poll (even for
 * ongoing sightings) so flight paths draw smoothly. */
bool fw_an_observe_ex(FwAnalytics *a, const FwAnObs *obs, uint32_t now, int utc_offset_min);

/* Backwards-compatible shorthand (airline only). */
bool fw_an_observe(FwAnalytics *a, uint32_t key, const char *airline, uint32_t now, int utc_offset_min);

/* Async route backfill (e.g. community feed adsbdb lookup arriving after the
 * initial sighting): counts orig/dest once per sighting and enriches any
 * matching recent rare sighting. */
void fw_an_route(FwAnalytics *a, uint32_t key, const char *orig, const char *dest, uint32_t now, int utc_offset_min);

/* Plots a position fix for an aircraft onto the flight-path heatmap. */
void fw_an_position(FwAnalytics *a, uint32_t key, double lat, double lon, uint32_t now);

/* The board was watching during the minute containing `now` (call once per
 * feed poll that succeeded); minutes never marked are coverage gaps. */
void fw_an_covered(FwAnalytics *a, uint32_t now, int utc_offset_min);

/* Read-out for a local hour or day (NULL when not held). */
const FwAnHour *fw_an_hour(const FwAnalytics *a, int32_t hour);
const FwAnDay *fw_an_day(const FwAnalytics *a, int32_t day);
int32_t fw_an_local_hour(uint32_t now, int utc_offset_min);
int32_t fw_an_local_day(uint32_t now, int utc_offset_min);

/* A day's ranked airlines, types, origins or destinations (most first, ties:
 * code order). Returns the count. */
int fw_an_top_airlines(const FwAnalytics *a, int32_t day, FwAnAirline out[FW_AN_TOP]);
int fw_an_top_types(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]);
int fw_an_top_origins(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]);
int fw_an_top_dests(const FwAnalytics *a, int32_t day, FwAnTag out[FW_AN_DIM_TOP]);

/* Streaming JSON export (zero heap): emits the complete analytics JSON object
 * in small chunks via `emit`. */
typedef void (*FwAnEmit)(void *ctx, const char *data, size_t len);
void fw_an_json_stream(const FwAnalytics *a, FwAnEmit emit, void *ctx);

/* Buffer wrapper around fw_an_json_stream; returns the full length needed
 * (like snprintf). */
size_t fw_an_json(const FwAnalytics *a, char *buf, size_t cap);

/* Checkpoint: hours, days (airlines, types, airports), rare sightings,
 * heatmap and totals (not the dedup/track tables), versioned, with CRC-32.
 * fw_an_save returns bytes written (0 if cap too small); fw_an_load returns
 * false, leaving `a` untouched, on any mismatch. */
#define FW_AN_BLOB_MAX 12288
size_t fw_an_save(const FwAnalytics *a, uint8_t *buf, size_t cap);
bool fw_an_load(FwAnalytics *a, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
