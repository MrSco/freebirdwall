#ifndef FW_AERO_H
#define FW_AERO_H
/* FlightAware AeroAPI with the owner's own key, for tracked flights on the
 * community source (FEED.md "Tracked flights on the community source"):
 * GET /aeroapi/flights/{ident} gives the times and status, and, only when
 * adsb.fi cannot see the aircraft, GET /aeroapi/flights/{fa_flight_id}/position
 * gives where it is. This reads the two answers. Portable C++17 (ArduinoJson). */
#include <stddef.h>
#include <stdint.h>
#include "flight_data.h"     /* ArduinoJson */

struct FwAeroFlight {
    bool airborne;           /* actual_off without actual_on */
    bool landed;             /* actual_on */
    uint32_t dep, arr;       /* take-off and landing, unix s: actual, else estimated, else scheduled (runway
                              * times; gate times when those are missing); 0 unknown */
    double progress;         /* progress_percent, NAN when absent */
    char fa_id[64];
    char orig_iata[8], orig_icao[8], dest_iata[8], dest_icao[8];
    char orig[32], dest[32]; /* airport names as FlightAware gives them */
    char ac_type[16], reg[20];
};

/* Only the fields read, for deserializeJson's filter (keeps the document
 * small while a long answer streams past). */
void fw_aero_flights_filter(JsonDocument &filter);
void fw_aero_position_filter(JsonDocument &filter);

/* The flight to follow from a /flights/{ident} answer at now: one in the air;
 * else one that landed in the last 30 minutes; else the next to leave (not
 * cancelled; a delayed one up to 3 hours after its scheduled time). False
 * when there is none. */
bool fw_aero_pick(JsonVariantConst answer, uint32_t now, FwAeroFlight *out);

/* A /flights/{id}/position answer's last_position: metres, m/s, degrees.
 * False without latitude and longitude. */
bool fw_aero_position(JsonVariantConst answer, double *lat, double *lon, double *alt_m, double *gs_mps,
                      double *heading);

/* "2026-10-20T14:05:00Z" (or with an offset, or fractional seconds) -> unix
 * seconds; 0 when absent or not a time. */
uint32_t fw_aero_time(const char *iso);

#endif
