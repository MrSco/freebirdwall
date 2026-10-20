#ifndef FW_ANALYTICS_H
#define FW_ANALYTICS_H
/* Bounded on-device flight analytics (OWNED-FIRMWARE-PLAN.md step 4,
 * ANALYTICS.md). Not in the stock firmware ("coming soon" in Plus): an owned
 * design.
 *
 * Every flight the feed reports is recorded before the display picks its
 * five, deduplicated into sightings (a flight seen again within
 * FW_AN_REVISIT_S is the same sighting). Kept: sightings per local hour for
 * two days and per local day for a month, the top airlines of each day, and
 * how many minutes of each hour were actually observed, so sleep and outages
 * show as gaps instead of quiet hours. Fixed size, no allocation; the whole
 * state serialises to a small checksummed blob for batched saving.
 *
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
#define FW_AN_CODE 4                 /* airline code, 3 characters + NUL */

typedef struct {
    char code[FW_AN_CODE];
    uint16_t count;                  /* may overcount by at most `error` */
    uint16_t error;
} FwAnAirline;

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
    FwAnAirline top[FW_AN_TOP];
} FwAnDay;

typedef struct {
    uint32_t key;                    /* flight identity hash, 0 = empty */
    uint32_t last_seen;
} FwAnRecent;

typedef struct {
    FwAnHour hours[FW_AN_HOURS];     /* indexed by hour % FW_AN_HOURS */
    FwAnDay days[FW_AN_DAYS];        /* indexed by day % FW_AN_DAYS */
    FwAnRecent recent[FW_AN_RECENT];
    uint32_t total_sightings;
    uint32_t total_observations;
} FwAnalytics;

void fw_an_init(FwAnalytics *a);

/* Identity of a flight for deduplication: the ICAO hex address when known,
 * else the callsign, else the registration (FNV-1a, never 0). */
uint32_t fw_an_flight_key(const char *hex, const char *callsign, const char *reg);

/* One flight seen in a feed response. Returns true when it starts a new
 * sighting (and was counted). */
bool fw_an_observe(FwAnalytics *a, uint32_t key, const char *airline, uint32_t now, int utc_offset_min);

/* The board was watching during the minute containing `now` (call once per
 * feed poll that succeeded); minutes never marked are coverage gaps. */
void fw_an_covered(FwAnalytics *a, uint32_t now, int utc_offset_min);

/* Read-out for a local hour or day (NULL when not held). */
const FwAnHour *fw_an_hour(const FwAnalytics *a, int32_t hour);
const FwAnDay *fw_an_day(const FwAnalytics *a, int32_t day);
int32_t fw_an_local_hour(uint32_t now, int utc_offset_min);
int32_t fw_an_local_day(uint32_t now, int utc_offset_min);

/* A day's airlines, most first (ties: code order). Returns the count. */
int fw_an_top_airlines(const FwAnalytics *a, int32_t day, FwAnAirline out[FW_AN_TOP]);

/* JSON for the browser charts: {"hours":[[hour,sightings,minutes],..],
 * "days":[[day,sightings,minutes,[["AAL",n],..]],..]}, oldest first.
 * Returns the full length (like snprintf). */
size_t fw_an_json(const FwAnalytics *a, char *buf, size_t cap);

/* Checkpoint: hours, days and totals (not the dedup table), versioned, with
 * a CRC-32. fw_an_save returns the bytes written (0 if cap is too small);
 * fw_an_load returns false, leaving `a` untouched, on any mismatch. */
#define FW_AN_BLOB_MAX 4096
size_t fw_an_save(const FwAnalytics *a, uint8_t *buf, size_t cap);
bool fw_an_load(FwAnalytics *a, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
