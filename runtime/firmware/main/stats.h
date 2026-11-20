#ifndef FLIGHTWALL_STATS_H
#define FLIGHTWALL_STATS_H
/* Flight stats on the board (ANALYTICS.md): every flight a feed poll reports
 * goes into components/fw_analytics, deduplicated into sightings per local
 * hour and day, with top airlines, aircraft types, departure/arrival airports,
 * rare aircraft badges, and a flight-path heatmap. Kept in memory (with
 * optional LAN/NAS HTTP backup server and browser checkpoint download/restore).
 * Logs nothing about flights or coordinates. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void fw_stats_init(void);

/* Sets the watch area(s) for the flight-path heatmap (single circle or
 * enclosing lat/lon box covering all configured areas). */
void fw_stats_set_area(double lat, double lon, double radius_km);
void fw_stats_set_bounds(double lat_min, double lat_max, double lon_min, double lon_max);

/* Sets the optional LAN/NAS backup server URL ("http://host[:port][/path]" or ""). */
void fw_stats_set_sync_url(const char *url);

/* Before a poll's flights: takes the time and the local UTC offset (the
 * owned settings' zone, with daylight saving). False while the clock is not
 * set; then the flights are not counted. */
bool fw_stats_begin(void);

/* One flight seen by the poll (shorthand without type/route/position). */
void fw_stats_flight(const char *hex, const char *callsign, const char *reg, const char *airline);

/* One flight with full analytics fields: aircraft type, origin/dest airports
 * (IATA preferred, else ICAO), and optional position for the heatmap. */
void fw_stats_flight_ex(const char *hex, const char *callsign, const char *reg, const char *airline,
                        const char *ac_type, const char *orig, const char *dest,
                        bool has_pos, double lat, double lon);

/* Async route backfill (e.g. community feed adsbdb lookup). */
void fw_stats_route(const char *hex, const char *callsign, const char *reg, const char *orig, const char *dest);

/* Standalone position fix (e.g. vendor feed progress-bar adsb.fi lookup). */
void fw_stats_position(const char *hex, const char *callsign, const char *reg, double lat, double lon);

/* The poll succeeded: this minute was watched. */
void fw_stats_covered(void);

/* Streaming JSON callback for zero-heap HTTP responses. Return false on write error. */
typedef bool (*FwStatsEmit)(void *ctx, const char *data, size_t len);
bool fw_stats_stream(FwStatsEmit emit, void *ctx);

/* Buffer form (for tests/small callers); returns length needed (like snprintf). */
size_t fw_stats_json(char *buf, size_t cap);

/* Binary CRC-32 checkpoint save/load (for browser download/restore and LAN backup). */
size_t fw_stats_checkpoint_save(uint8_t *buf, size_t cap);
bool fw_stats_checkpoint_load(const uint8_t *buf, size_t len);

/* Optional LAN/NAS HTTP backup sync (runs on feed_task). */
typedef struct {
    bool configured;
    bool pending;
    bool ok;
    bool restored;
    int status;
    uint32_t last_at;
} FwStatsSyncStatus;

void fw_stats_sync_request(void);          /* trigger an immediate backup on feed_task */
void fw_stats_sync_step(void);             /* called periodically from feed_task */
void fw_stats_sync_status(FwStatsSyncStatus *out);

#ifdef __cplusplus
}
#endif
#endif
