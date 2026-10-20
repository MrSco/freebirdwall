#ifndef FW_COMMUNITY_FRAME_H
#define FW_COMMUNITY_FRAME_H
/* Community flights (adsb.fi positions, adsbdb routes; FEED.md "Community
 * feed") as the screens' flight list. The flightwall:: records come from the
 * portable reader in runtime/src; this turns the nearest ones into
 * the same FwFeedFrame the vendor feed produces, so screens, logos and the
 * owner's airport names work unchanged. Portable C++17. */
#include <stddef.h>
#include "flight_data.h"
#include "fw_feed.h"

/* At most FW_FEED_MAX_FLIGHTS rows from the start of batch (already ordered
 * nearest first); overflow = the rest. Logos are looked up through logos. */
void fw_community_frame(const flightwall::Batch &batch, const flightwall::LogoCatalog &logos, FwFeedFrame *out);

/* A Sync leg tracked live (SYNC.md "Owned Sync"): fills f's tracked fields
 * from its position (f must hold it: lat/lon given separately), the airports'
 * coordinates when known (coords) and the scheduled times: prog (percent of
 * the great-circle way flown, else from the times), dist (metres flown, NAN
 * without coordinates), dep ("35m ago", the scheduled departure), arr (time
 * left at the current ground speed, else to the scheduled arrival), trk_gnd;
 * landed (prog 100) once on the ground within 10 km of the destination. */
void fw_community_track(FwFlight *f, double lat, double lon, bool coords, double olat, double olon, double dlat,
                        double dlon, uint32_t dep_at, uint32_t arr_at, uint32_t now);

/* Great-circle distance in metres. */
double fw_community_distance(double lat1, double lon1, double lat2, double lon2);

/* One record as the screens' flight (used for a Sync leg tracked live). */
void fw_community_flight(const flightwall::Flight &f, const flightwall::LogoCatalog &logos, FwFlight *out);

/* "London Heathrow Airport" -> "London Heathrow", "John F Kennedy International
 * Airport" -> "John F Kennedy Intl": shorter names, in the vendor's style. */
void fw_community_airport_name(const char *in, char *out, size_t cap);

/* "BOEING 737-800" -> "Boeing 737-800": all-capital words longer than three
 * letters (and DE, as in DE HAVILLAND) are written in title case; codes such
 * as ATR, A-320 or 300ER are kept. */
void fw_community_aircraft_name(const char *in, char *out, size_t cap);

#endif
