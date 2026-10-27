#pragma once
/* One aircraft's position by its ICAO address, from adsb.fi (position_fix.cpp):
 * for the progress bar on the FlightWall feed. Feed task only. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* hex: 6 hex digits. 0 with a fresh position (seen within a minute); else
 * an https_get.h stage (1..6), 10 another HTTP status, 11 no fresh position,
 * -1 a bad hex. *status is the HTTP status. */
int fw_position_fix(const char *hex, double *lat, double *lon, bool *ground, unsigned *status);

#ifdef __cplusplus
}
#endif
