#include "fw_airports.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

extern const unsigned fw_airport_count;
extern const uint8_t fw_airport_table[];

#define RECORD 10

static int nan_(double v){ return v != v; }

static double s24(const uint8_t *p){
    int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16);
    if (v & 0x800000) v -= 0x1000000;
    return v / 10000.0;
}

bool fw_airport_position(const char *icao, double *lat, double *lon){
    char key[4];
    unsigned lo = 0, hi = fw_airport_count;
    if (!icao) return false;
    for (int i = 0; i < 4; ++i) {
        if (!isalpha((unsigned char)icao[i])) return false;
        key[i] = (char)toupper((unsigned char)icao[i]);
    }
    if (icao[4]) return false;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        const uint8_t *r = fw_airport_table + (size_t)mid * RECORD;
        int c = memcmp(r, key, 4);
        if (!c) {
            *lat = s24(r + 4);
            *lon = s24(r + 7);
            return true;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

/* fw_community_distance: haversine on the mean Earth radius */
static double distance(double lat1, double lon1, double lat2, double lon2){
    const double r = 6371008.8, k = 3.14159265358979323846 / 180.0;
    double dlat = (lat2 - lat1) * k, dlon = (lon2 - lon1) * k;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * k) * cos(lat2 * k) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * r * atan2(sqrt(a), sqrt(1 - a));
}

double fw_route_progress(double olat, double olon, double dlat, double dlon, double lat, double lon, bool ground){
    double route, flown, left;
    if (nan_(olat) || nan_(olon) || nan_(dlat) || nan_(dlon) || nan_(lat) || nan_(lon)) return NAN;
    route = distance(olat, olon, dlat, dlon);
    flown = distance(olat, olon, lat, lon);
    left = distance(lat, lon, dlat, dlon);
    if (route < 50000 || flown + left > 1.5 * route) return NAN;
    if (ground && left < 10000) return 100;
    if (ground && flown < 10000) return 0;
    return 100 * flown / (flown + left);
}
