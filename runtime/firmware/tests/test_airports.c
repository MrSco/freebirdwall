/* The airport table and the route progress (components/fw_airports,
 * FLIGHT-SCREENS.md "Progress bar"). Run by tests/run_host_tests.py. */
#include <math.h>
#include <stdio.h>
#include "fw_airports.h"

static int checks, failed;
#define CHECK(c, what) do { ++checks; if (!(c)) { ++failed; printf("FAIL %s\n", what); } } while (0)

static int near(double a, double b, double tol){ return fabs(a - b) <= tol; }
static int none(double v){ return v != v; }

int main(void){
    double lat, lon, p;
    /* lookups: OurAirports' coordinates to 1/10,000 degree */
    CHECK(fw_airport_position("KJFK", &lat, &lon) && near(lat, 40.6394, 0.0001) && near(lon, -73.7793, 0.0001), "KJFK");
    CHECK(fw_airport_position("egll", &lat, &lon) && near(lat, 51.4707, 0.0001) && near(lon, -0.4599, 0.0001), "egll, lower case");
    CHECK(fw_airport_position("TJSJ", &lat, &lon) && near(lat, 18.4394, 0.0001), "TJSJ");
    CHECK(fw_airport_position("YSSY", &lat, &lon) && lat < -33 && lon > 151, "YSSY: southern, eastern");
    CHECK(fw_airport_position("KDJT", &lat, &lon) && near(lat, 26.6832, 0.0001), "Palm Beach, renamed: KDJT");
    CHECK(fw_airport_position("KPBI", &lat, &lon) && near(lat, 26.6832, 0.0001), "Palm Beach, renamed: still KPBI");
    CHECK(fw_airport_position("KBCT", &lat, &lon), "KBCT, a smaller airport");
    CHECK(!fw_airport_position("ZZZZ", &lat, &lon), "unknown code");
    CHECK(!fw_airport_position("JFK", &lat, &lon) && !fw_airport_position("KJFKX", &lat, &lon) &&
          !fw_airport_position("", &lat, &lon) && !fw_airport_position(NULL, &lat, &lon) &&
          !fw_airport_position("K1FK", &lat, &lon), "not four letters");
    CHECK(fw_airport_position("AAAA", &lat, &lon) || !fw_airport_position("AAAA", &lat, &lon), "first code: no crash");
    CHECK(!fw_airport_position("ZZZZZ", &lat, &lon), "past the end");

    /* progress: halfway along the equator is 50 */
    p = fw_route_progress(0, 0, 0, 10, 0, 5, false);
    CHECK(near(p, 50, 0.01), "halfway");
    CHECK(near(fw_route_progress(0, 0, 0, 10, 0, 0, false), 0, 0.01), "at the origin");
    CHECK(near(fw_route_progress(0, 0, 0, 10, 0, 10, false), 100, 0.01), "at the destination");
    CHECK(near(fw_route_progress(0, 0, 0, 10, 1, 2.5, false), 26.23, 0.05), "a little off the line");
    CHECK(none(fw_route_progress(0, 0, 0, 10, 20, 5, false)), "far off the route: none");
    CHECK(none(fw_route_progress(0, 0, 0, 0.3, 0, 0.1, false)), "airports under 50 km apart: none");
    CHECK(none(fw_route_progress(NAN, 0, 0, 10, 0, 5, false)), "no position: none");
    CHECK(fw_route_progress(0, 0, 0, 10, 0, 0.05, true) == 0, "on the ground by the origin: 0");
    CHECK(fw_route_progress(0, 0, 0, 10, 0, 9.95, true) == 100, "on the ground by the destination: 100");
    CHECK(near(fw_route_progress(0, 0, 0, 10, 0, 5, true), 50, 0.01), "on the ground elsewhere: as in the air");
    /* across the date line: Tokyo Haneda to Honolulu, the aircraft east of 180 */
    if (fw_airport_position("RJTT", &lat, &lon)) {
        double hlat, hlon;
        CHECK(fw_airport_position("PHNL", &hlat, &hlon), "PHNL");
        p = fw_route_progress(lat, lon, hlat, hlon, 30, -175, false);
        CHECK(p > 50 && p < 90, "across the date line");
    }
    {   /* on the ground, no position: the airport in an area tells (MIA area, Fort Lauderdale area) */
        const FwWatchArea mia[1] = {{25.7959f, -80.2870f, 3.2f}}, both[2] = {{25.7959f, -80.2870f, 3.2f}, {26.0726f, -80.1527f, 5}};
        CHECK(fw_ground_progress("KMIA", "TJSJ", mia, 1) == 0, "ground: at its origin, 0");
        CHECK(fw_ground_progress("EGLL", "KMIA", mia, 1) == 100, "ground: at its destination, 100");
        CHECK(none(fw_ground_progress("KFLL", "KMIA", both, 2)), "ground: both airports in areas, none");
        CHECK(none(fw_ground_progress("KATL", "KJFK", mia, 1)), "ground: neither airport in an area, none");
        CHECK(none(fw_ground_progress("KMIA", "", mia, 1)) && none(fw_ground_progress("KMIA", "ZZZZ", mia, 1)), "ground: unknown airport, none");
        CHECK(none(fw_ground_progress("KMIA", "TJSJ", mia, 0)), "ground: no areas, none");
    }
    printf("%s: %d airport checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed != 0;
}
