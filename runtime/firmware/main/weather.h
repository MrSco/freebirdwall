#ifndef FLIGHTWALL_WEATHER_H
#define FLIGHTWALL_WEATHER_H
/* The weather "when the sky is quiet" on the board (WEATHER.md). One record,
 * shared by the feed task (which fills it) and the display (which shows it):
 *  - the FlightWall feed's own "weather" record, kept by the vendor's rules
 *    (a response with flights keeps the last one; without, it replaces or
 *    clears it);
 *  - otherwise (the community source, or a FlightWall feed that sends none
 *    while weather is on) the METAR from aviationweather.gov, read here on
 *    the feed task: the owner's station or the one nearest the first area,
 *    every 10 minutes while weather is on. */
#include <stdbool.h>
#include <stdint.h>
#include "fw_weather.h"
#ifdef __cplusplus
extern "C" {
#endif

void fw_weather_store_init(void);

/* A good FlightWall response: its weather record (may be invalid) and whether it listed flights. */
void fw_weather_store_vendor(const FwWeather *w, bool has_flights);

/* The record now held (fw_weather_valid false when none); returns a serial that changes with it. */
uint32_t fw_weather_store_copy(FwWeather *out);

typedef struct {
    bool enabled;              /* display_config.weather.enabled */
    bool community;            /* the community source (else the FlightWall feed) */
    char station[6];           /* owned_config.weather_station, "" = nearest */
    bool has_area;
    float lat, lon;            /* the first area's centre */
} FwWeatherWant;

/* On the feed task, between polls: one aviationweather.gov request when due. */
void fw_weather_step(const FwWeatherWant *want);

/* For the dashboard: where the record came from ("feed", "aviationweather",
 * "" none), the station, the last request's HTTP status and counts. */
void fw_weather_status(char *source, int source_cap, char *station, int station_cap, unsigned *status,
                       unsigned *requests, unsigned *failures);

#ifdef __cplusplus
}
#endif
#endif
