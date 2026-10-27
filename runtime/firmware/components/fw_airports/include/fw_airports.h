#ifndef FW_AIRPORTS_H
#define FW_AIRPORTS_H
/* Airport locations by four-letter (ICAO) code, from OurAirports
 * (airports_table.c, tools/make_airports.py): every large, medium and small
 * airport with such a code, in flash. Portable C. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The airport's latitude and longitude in degrees (to about 11 m); false when
 * the code is not four letters or not in the table. Case does not matter. */
bool fw_airport_position(const char *icao, double *lat, double *lon);

/* Owned (FLIGHT-SCREENS.md "Progress bar"): how far along its route a flight
 * is, 0..100: the great-circle distance flown from the origin over the
 * distance flown plus the distance left, as the tracked card works it out
 * (fw_community_track). On the ground near an airport: 0 at the origin, 100
 * at the destination. NAN when the route is unusable: the airports closer
 * than 50 km, or the aircraft more than half again the route's length off
 * it (flown + left > 1.5 x the route: a wrong or stale route). */
double fw_route_progress(double olat, double olon, double dlat, double dlon, double lat, double lon, bool ground);

#ifdef __cplusplus
}
#endif
#endif
