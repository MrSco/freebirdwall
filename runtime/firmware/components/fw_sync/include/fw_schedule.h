#ifndef FW_SCHEDULE_H
#define FW_SCHEDULE_H
/* The owned Sync schedule (SYNC.md "Owned Sync"): the legs a crew member (or a
 * traveller) loads on the dashboard, kept on the wall, turned each minute into
 * what stock's Plus Sync screens show: the active leg, up to five upcoming
 * ones, and the relative times as text stock's rules read ("in 2h 05m",
 * "in 3d", "35m ago"). Not the vendor's service: the vendor fills the same
 * record from its server; here the wall works it out from its own clock.
 *
 * Saved only when the owner saves on the dashboard (no routine flash writes).
 * Portable C. Times are unix seconds (UTC). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_sync.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_SCHED_MAX 20
#define FW_SCHED_ACTIVE_BEFORE_S (2 * 3600)   /* a leg is active from 2 h before departure */
#define FW_SCHED_ACTIVE_AFTER_S (30 * 60)     /* ... until 30 min after arrival */
/* Overlaps: a leg in the air first, then one that landed under 30 min ago,
 * then the next within 2 h. Canceled legs are never active. */

typedef enum { FW_LEG_OPERATING = 0, FW_LEG_DEADHEAD = 1, FW_LEG_RESERVE = 2 } FwLegType;

typedef struct {
    char al[4];          /* airline code as entered: IATA "AA" or ICAO "AAL" */
    char fn[6];          /* flight number digits (and a letter suffix), "1234" */
    char orig[5], dest[5];   /* airport codes, IATA or ICAO */
    uint8_t type;        /* FwLegType */
    uint8_t canceled;
    uint32_t dep_at, arr_at;   /* scheduled, UTC */
} FwSchedLeg;

typedef struct {
    bool enabled;
    bool show_upcoming;          /* stock plus.sync.display.show_upcoming (default on) */
    bool message_view;           /* main_view "message" (else "active") */
    bool list_view;              /* upcoming_view "list" (else "next") */
    bool show_deadhead, show_reserve;
    int count;
    FwSchedLeg legs[FW_SCHED_MAX];
    char message[FW_SYNC_MESSAGE_MAX];
} FwSchedule;

void fw_sched_init(FwSchedule *s);

/* What the screens show at `now`. */
typedef struct {
    bool has_active;
    FwSyncLeg active;
    int upcoming_count;
    FwSyncLeg upcoming[FW_SYNC_UPCOMING_MAX];
    const char *message;         /* NULL when empty */
} FwSchedView;

void fw_sched_view(const FwSchedule *s, uint32_t now, FwSchedView *out);

/* The three-letter (ICAO) airline code for a code a crew member types:
 * "AA" -> "AAL"; a three-letter code is returned as it is; an unknown
 * two-character code gives NULL. Logos and live tracking use ICAO codes. */
const char *fw_sched_icao_airline(const char *code);

/* The callsign a flight number flies under: "AA1234" -> "AAL1234" (a
 * two-character airline code becomes its ICAO code, the number loses leading
 * zeros: "AA0012" -> "AAL12"); "AAL1234" (three letters first) is kept; an
 * unknown code is kept as typed. */
void fw_sched_callsign(const char *number, char *cs, size_t cap);

/* While a leg is worth tracking live (SYNC.md "Live trip"): not canceled,
 * from 30 min before departure until 30 min after arrival (6 h after
 * departure when the arrival is unknown). */
#define FW_SCHED_LIVE_MARGIN 1800
bool fw_sched_live_window(const FwSyncLeg *leg, uint32_t now);

/* "in 2h 05m" / "in 45m" / "in 3d" before t, "35m ago" / "2h 05m ago" /
 * "2d ago" after (whole minutes; "in 0m" at t). */
void fw_sched_rel(uint32_t t, uint32_t now, char *out, size_t cap);

/* JSON round trip for the dashboard (GET/PUT /api/sync): {"enabled":..,
 * "show_upcoming":..,"main_view":"active|message","upcoming_view":"next|list",
 * "show_deadhead":..,"show_reserve":..,"message":"..","legs":[{"al","fn",
 * "orig","dest","dep","arr" (ISO-8601 UTC),"type":"operating|deadhead|reserve",
 * "canceled":bool}]}. Parsing validates everything and returns false (with a
 * reason) without touching *out on any error; legs are kept sorted by
 * departure. */
size_t fw_sched_json(const FwSchedule *s, char *buf, size_t cap);
bool fw_sched_parse(const char *json, FwSchedule *out, char *why, size_t why_cap);

/* Compact storage record (one NVS blob), versioned with a CRC; load refuses
 * anything else. */
#define FW_SCHED_BLOB_MAX 1200
size_t fw_sched_save(const FwSchedule *s, uint8_t *buf, size_t cap);
bool fw_sched_load(FwSchedule *s, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
