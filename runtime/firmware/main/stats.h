#ifndef FLIGHTWALL_STATS_H
#define FLIGHTWALL_STATS_H
/* Flight stats on the board (ANALYTICS.md): every flight a feed poll reports
 * goes into components/fw_analytics, deduplicated into sightings per local
 * hour and day, with the minutes actually watched. Kept in memory only (lost
 * on a restart). Logs nothing about flights. */
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void fw_stats_init(void);

/* Before a poll's flights: takes the time and the local UTC offset (the
 * owned settings' zone, with daylight saving). False while the clock is not
 * set; then the flights are not counted. */
bool fw_stats_begin(void);

/* One flight seen by the poll. airline: a three-letter code or NULL; when
 * missing, a callsign like DAL1234 gives DAL. */
void fw_stats_flight(const char *hex, const char *callsign, const char *reg, const char *airline);

/* The poll succeeded: this minute was watched. */
void fw_stats_covered(void);

/* {"hour":<local hour now>,"day":<local day now>,"offset":<minutes>,
 *  "stats":<fw_an_json>}; hour/day -1 while the clock is not set. Returns the
 * length needed (like snprintf); complete only when less than cap. */
size_t fw_stats_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
