#ifndef FLIGHTWALL_COMMUNITY_H
#define FLIGHTWALL_COMMUNITY_H
/* The community flight source (FEED.md "Community feed"): positions from
 * adsb.fi, routes from adsbdb, over the source-built TLS engine. Runs on the
 * feed task; feed.c chooses it when the owned settings say so. Logs counts
 * only, never positions, callsigns or the owner's areas. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_feed.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The TLS client's context (its arena is the shared block, big_buffer.h);
 * also used by the route probe in probe builds, never at the same time. */
extern uint8_t fw_tls_context[];
extern const size_t fw_tls_context_bytes_reserved;

typedef struct {
    int areas;             /* areas in use (0: nothing to watch) */
    int tracked;           /* tracked flights looked for (FEED.md) */
    int skipped_geo;       /* drawn areas left out (vendor source only) */
} FwCommunityInfo;

/* Reads request_config from the settings document; clears what was cached.
 * Returns false when there is no usable area. */
bool fw_community_configure(const char *settings_json, FwCommunityInfo *info);

/* One step: at most one position request (5 s apart, shared by all areas)
 * and a few route lookups, then the nearest flights into frame. Returns the
 * milliseconds to wait before the next step. *good is set once a position
 * request has succeeded since configure. */
uint32_t fw_community_step(FwFeedFrame *frame, bool *good);

/* FlightAware with the owner's key, since start (counts only). */
typedef struct {
    bool in_use;              /* tracking_source "aeroapi" with a key saved */
    bool rejected;            /* FlightAware refused the key (401/403): no requests until it changes */
    unsigned flights_calls;   /* /flights/{ident}, $0.005 each on the Personal tier */
    unsigned position_calls;  /* /flights/{id}/position, $0.01 each */
    unsigned last_status;     /* HTTP status of the last request */
} FwAeroStatus;
void fw_community_aero_status(FwAeroStatus *out);

/* The Sync leg in the air, tracked live by callsign (SYNC.md "Owned Sync"):
 * true and *out filled when the aircraft with this ICAO callsign was found in
 * the last minute (position, metrics, progress, departed/arriving texts). */
bool fw_community_live(const char *callsign, FwFlight *out);

#ifdef __cplusplus
}
#endif
#endif
