/* Desktop tests for components/fw_analytics.
 * Run: py tests/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_analytics.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_, b_); exit(1); } ++checks; } while (0)

static FwAnalytics A, B;

#define T0 1792411200u   /* 2026-10-19 12:00:00 UTC = 08:00 in New York (EDT, -240) */
#define EDT (-240)

static void tag_str(const FwAnTag *t, char out[5]){
    memcpy(out, t->code, 4);
    out[4] = 0;
}

static void test_keys(void){
    uint32_t a = fw_an_flight_key("a1b2c3", "AAL1", "N1"), b = fw_an_flight_key("a1b2c3", "DAL9", NULL);
    CHECK(a && a == b);                                              /* hex wins */
    CHECK(fw_an_flight_key(NULL, "AAL1", "N1") == fw_an_flight_key("", "AAL1", "N2"));
    CHECK(fw_an_flight_key(NULL, "AAL1", NULL) != fw_an_flight_key(NULL, NULL, "AAL1"));   /* kinds differ */
    CHECK(fw_an_flight_key(NULL, NULL, NULL) == 0 && fw_an_flight_key("", "", "") == 0);
}

static void test_local_time(void){
    CHECK(fw_an_local_hour(T0, 0) == (int32_t)(T0 / 3600));
    CHECK(fw_an_local_hour(T0, EDT) == (int32_t)(T0 / 3600) - 4);
    CHECK(fw_an_local_day(T0, EDT) == (int32_t)(T0 / 86400));       /* 08:00 local, same date */
    CHECK(fw_an_local_day(T0 - 9 * 3600, EDT) == (int32_t)(T0 / 86400) - 1);   /* 23:00 the day before */
    CHECK(fw_an_local_day(T0 + 13 * 3600, 330) == (int32_t)(T0 / 86400) + 1);  /* India: next day */
}

static void test_dedup(void){
    uint32_t k = fw_an_flight_key("abc123", NULL, NULL);
    fw_an_init(&A);
    CHECK(fw_an_observe(&A, k, "AAL", T0, EDT));
    CHECK(!fw_an_observe(&A, k, "AAL", T0 + 60, EDT));
    CHECK(!fw_an_observe(&A, k, "AAL", T0 + 60 + 1799, EDT));       /* refreshed each time it is seen */
    CHECK(fw_an_observe(&A, k, "AAL", T0 + 60 + 1799 + 1800, EDT));  /* gone 30 min: a new sighting */
    CHECK(A.total_sightings == 2 && A.total_observations == 4);
    CHECK(!fw_an_observe(&A, 0, "AAL", T0, EDT));                    /* no identity: not counted */
    {
        const FwAnHour *h = fw_an_hour(&A, fw_an_local_hour(T0, EDT));
        CHECK(h && h->sightings == 1);
    }
    /* the table is bounded: thousands of flights, the oldest forgotten first */
    fw_an_init(&A);
    for (uint32_t i = 0; i < 5000; ++i) {
        char hex[16];
        snprintf(hex, sizeof hex, "%06x", i);
        CHECK(fw_an_observe(&A, fw_an_flight_key(hex, NULL, NULL), "UAL", T0 + i, 0));
    }
    {
        char hex[16];
        int remembered = 0;
        for (uint32_t i = 4900; i < 5000; ++i) {     /* the most recent are still known */
            snprintf(hex, sizeof hex, "%06x", i);
            remembered += !fw_an_observe(&A, fw_an_flight_key(hex, NULL, NULL), "UAL", 5000 + T0 + 10, 0);
        }
        CHECK(remembered == 100);
    }
}

static void test_hours_days_coverage(void){
    int32_t h0 = fw_an_local_hour(T0, EDT), d0 = fw_an_local_day(T0, EDT);
    fw_an_init(&A);
    for (int m = 0; m < 30; ++m) fw_an_covered(&A, T0 + (uint32_t)m * 60, EDT);
    for (int m = 0; m < 30; ++m) fw_an_covered(&A, T0 + (uint32_t)m * 60 + 30, EDT);   /* same minutes */
    CHECK(fw_an_hour(&A, h0)->minutes_observed == 30);
    CHECK(fw_an_day(&A, d0)->minutes_observed == 30);
    CHECK(fw_an_hour(&A, h0 + 1) == NULL);                           /* never seen: a gap, not zero */
    fw_an_covered(&A, T0 + 3600, EDT);
    CHECK(fw_an_hour(&A, h0 + 1)->minutes_observed == 1 && fw_an_hour(&A, h0 + 1)->sightings == 0);

    /* 48 hours kept; older hours are replaced */
    for (int i = 0; i < 60; ++i) {
        char cs[16];
        snprintf(cs, sizeof cs, "T%d", i);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), "AAL", T0 + (uint32_t)i * 3600, EDT);
    }
    CHECK(fw_an_hour(&A, h0) == NULL && fw_an_hour(&A, h0 + 11) == NULL);
    CHECK(fw_an_hour(&A, h0 + 12) && fw_an_hour(&A, h0 + 59)->sightings == 1);
    /* an observation older than what a slot holds is dropped from hours, kept in days */
    CHECK(fw_an_observe(&A, fw_an_flight_key(NULL, "OLD", NULL), "AAL", T0, EDT));
    CHECK(fw_an_hour(&A, h0) == NULL);
    /* days: 60 hours from 08:00 local spans 3 days (16 + 24 + 20 h) */
    CHECK(fw_an_day(&A, d0)->sightings == 16 + 1 && fw_an_day(&A, d0 + 1)->sightings == 24 &&
          fw_an_day(&A, d0 + 2)->sightings == 20);

    /* 31 days kept */
    fw_an_init(&A);
    for (int i = 0; i < 40; ++i) {
        char cs[16];
        snprintf(cs, sizeof cs, "D%d", i);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), "AAL", T0 + (uint32_t)i * 86400, EDT);
    }
    CHECK(fw_an_day(&A, d0 + 8) == NULL && fw_an_day(&A, d0 + 9) && fw_an_day(&A, d0 + 39));
}

static void test_top(void){
    FwAnAirline top[FW_AN_TOP];
    int32_t d0 = fw_an_local_day(T0, 0);
    static const char *codes[] = {"AAL", "DAL", "UAL", "SWA", "JBU"};
    static const int counts[] = {7, 3, 5, 3, 1};
    int n = 0;
    fw_an_init(&A);
    for (int c = 0; c < 5; ++c)
        for (int k = 0; k < counts[c]; ++k) {
            char cs[16];
            snprintf(cs, sizeof cs, "%s%d", codes[c], k);
            fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), c == 2 ? "ual" : codes[c], T0 + (uint32_t)n++, 0);
        }
    CHECK(fw_an_top_airlines(&A, d0, top) == 5);
    CHECK_STR(top[0].code, "AAL"); CHECK(top[0].count == 7);
    CHECK_STR(top[1].code, "UAL"); CHECK(top[1].count == 5);
    CHECK_STR(top[2].code, "DAL"); CHECK_STR(top[3].code, "SWA");
    CHECK_STR(top[4].code, "JBU");
    CHECK(fw_an_top_airlines(&A, d0 + 1, top) == 0);

    fw_an_init(&A);
    n = 0;
    for (int round = 0; round < 50; ++round) {
        char cs[16], code[8];
        snprintf(cs, sizeof cs, "BIG%d", round);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), "BIG", T0 + (uint32_t)n++, 0);
        snprintf(cs, sizeof cs, "R%d", round);
        snprintf(code, sizeof code, "Z%02d", round);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), code, T0 + (uint32_t)n++, 0);
    }
    CHECK(fw_an_top_airlines(&A, d0, top) == FW_AN_TOP);
    CHECK_STR(top[0].code, "BIG"); CHECK(top[0].count == 50 && top[0].error == 0);
    for (int i = 1; i < FW_AN_TOP; ++i) CHECK(top[i].count - top[i].error <= 1);
    CHECK(fw_an_day(&A, d0)->sightings == 100);
    fw_an_observe(&A, fw_an_flight_key(NULL, "NOAL", NULL), "", T0 + 500, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "NULLAL", NULL), NULL, T0 + 501, 0);
    CHECK(fw_an_day(&A, d0)->sightings == 102);
}

static void test_types_airports_rare_heat(void){
    FwAnTag types[FW_AN_DIM_TOP], orig[FW_AN_DIM_TOP], dest[FW_AN_DIM_TOP];
    char s[5];
    int32_t d0 = fw_an_local_day(T0, 0);
    uint32_t k1 = fw_an_flight_key("a00001", "BGA142", "F-GSTA");
    uint32_t k2 = fw_an_flight_key("a00002", "DAL402", "N813DN");
    uint32_t k3 = fw_an_flight_key("a00003", "JBU615", "N594JB");

    /* Rarity lookup */
    CHECK(fw_an_rarity("A3ST") == FW_AN_RARE_ONE_OF_KIND);
    CHECK(fw_an_rarity("a388") == FW_AN_RARE_VERY);
    CHECK(fw_an_rarity("B752") == FW_AN_RARE_NOTABLE);
    CHECK(fw_an_rarity("B738") == FW_AN_RARE_NONE);
    CHECK(fw_an_rarity("") == FW_AN_RARE_NONE && fw_an_rarity(NULL) == FW_AN_RARE_NONE);
    CHECK_STR(fw_an_rarity_label(FW_AN_RARE_ONE_OF_KIND), "One of a kind");

    fw_an_init(&A);
    fw_an_set_area(&A, 40.6413, -73.7781, 15.0);

    /* Flight 1: rare Beluga observed without route, then route backfilled asynchronously */
    {
        FwAnObs o = {k1, "BGA", "A3ST", NULL, NULL, "BGA142", "F-GSTA", true, 40.6413, -73.7781};
        CHECK(fw_an_observe_ex(&A, &o, T0, 0));
        /* Second position fix 30s later draws a line segment across the heatmap */
        o.lat = 40.70; o.lon = -73.70;
        CHECK(!fw_an_observe_ex(&A, &o, T0 + 30, 0));
    }
    CHECK(A.rare_count == 1 && A.rare_total == 1);
    CHECK_STR(A.rare[0].ac_type, "A3ST");
    CHECK_STR(A.rare[0].orig, "");
    fw_an_route(&A, k1, "TLS", "HAM", T0 + 40, 0);
    fw_an_route(&A, k1, "TLS", "HAM", T0 + 50, 0); /* duplicate backfill ignored */
    CHECK_STR(A.rare[0].orig, "TLS");
    CHECK_STR(A.rare[0].dest, "HAM");
    CHECK(fw_an_day(&A, d0)->routes_known == 1);

    /* Flights 2 & 3 with immediate routes */
    {
        FwAnObs o = {k2, "DAL", "B738", "ATL", "JFK", "DAL402", "N813DN", false, 0, 0};
        CHECK(fw_an_observe_ex(&A, &o, T0 + 60, 0));
        o.key = k3; o.airline = "JBU"; o.ac_type = "B738"; o.orig = "BOS"; o.dest = "JFK";
        CHECK(fw_an_observe_ex(&A, &o, T0 + 70, 0));
    }
    CHECK(fw_an_day(&A, d0)->routes_known == 3);
    CHECK(fw_an_top_types(&A, d0, types) == 2);
    tag_str(&types[0], s); CHECK_STR(s, "B738"); CHECK(types[0].count == 2);
    CHECK(fw_an_top_dests(&A, d0, dest) == 2);
    tag_str(&dest[0], s); CHECK_STR(s, "JFK"); CHECK(dest[0].count == 2);
    CHECK(fw_an_top_origins(&A, d0, orig) == 3);

    /* Heatmap should have multiple cells lit along the segment */
    {
        int lit = 0;
        for (int i = 0; i < FW_AN_GRID * FW_AN_GRID; ++i) lit += A.heat.cells[i] > 0;
        CHECK(lit >= 4 && A.heat.points == 2 && A.heat.since == T0);
    }
    /* Small area tweak keeps the heatmap; major relocation resets it */
    fw_an_set_area(&A, 40.642, -73.779, 15.5);
    CHECK(A.heat.points == 2);
    fw_an_set_area(&A, 51.47, -0.45, 15.0);
    CHECK(A.heat.points == 0 && A.heat.since == 0);
    /* Multi-area bounding box covers flights across a wide regional corridor */
    fw_an_set_bounds(&A, 25.75, 26.75, -80.35, -80.02);
    fw_an_position(&A, k1, 25.80, -80.28, T0 + 100); /* MIA */
    fw_an_position(&A, k2, 26.68, -80.09, T0 + 100); /* PBI */
    CHECK(A.heat.points == 2);
}

static void test_json(void){
    char buf[4096];
    size_t n;
    int32_t h0 = fw_an_local_hour(T0, 0), d0 = fw_an_local_day(T0, 0);
    char expect[512];
    fw_an_init(&A);
    CHECK_STR((fw_an_json(&A, buf, sizeof buf), buf), "{\"hours\":[],\"days\":[],\"rare\":[],\"rare_total\":0,\"heat\":null}");
    fw_an_covered(&A, T0, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "A1", NULL), "AAL", T0, 0);
    {
        FwAnObs o = {fw_an_flight_key(NULL, "A2", NULL), "AAL", "B738", "JFK", "LAX", "A2", NULL, false, 0, 0};
        fw_an_observe_ex(&A, &o, T0 + 3600, 0);
    }
    fw_an_observe(&A, fw_an_flight_key(NULL, "D1", NULL), "DAL", T0 + 3601, 0);
    n = fw_an_json(&A, buf, sizeof buf);
    snprintf(expect, sizeof expect,
             "{\"hours\":[[%ld,1,1],[%ld,2,0]],\"days\":[[%ld,3,1,[[\"AAL\",2],[\"DAL\",1]],1,[[\"B738\",1]],[[\"JFK\",1]],[[\"LAX\",1]]]],\"rare\":[],\"rare_total\":0,\"heat\":null}",
             (long)h0, (long)h0 + 1, (long)d0);
    CHECK_STR(buf, expect);
    CHECK(n == strlen(expect));
    {
        char small[10];
        CHECK(fw_an_json(&A, small, sizeof small) == n && strlen(small) == sizeof small - 1);
    }
}

static void test_checkpoint(void){
    static uint8_t blob[FW_AN_BLOB_MAX];
    static char j1[24576], j2[24576];
    size_t n;
    fw_an_init(&A);
    fw_an_set_area(&A, 40.6413, -73.7781, 20.0);
    for (int i = 0; i < 2000; ++i) {
        char cs[16];
        FwAnObs o;
        memset(&o, 0, sizeof o);
        snprintf(cs, sizeof cs, "F%d", i % 700);
        o.key = fw_an_flight_key(NULL, cs, NULL);
        o.airline = (i % 3) ? "AAL" : "SWA";
        o.ac_type = (i == 42) ? "A3ST" : (i % 2) ? "B738" : "A321";
        o.orig = (i % 2) ? "JFK" : "LGA";
        o.dest = (i % 3) ? "MIA" : "BOS";
        o.callsign = cs;
        o.has_pos = true;
        o.lat = 40.60 + (i % 20) * 0.005;
        o.lon = -73.85 + ((i / 20) % 20) * 0.006;
        fw_an_covered(&A, T0 + (uint32_t)i * 97, EDT);
        fw_an_observe_ex(&A, &o, T0 + (uint32_t)i * 97, EDT);
    }
    n = fw_an_save(&A, blob, sizeof blob);
    CHECK(n > 0 && n <= FW_AN_BLOB_MAX);
    CHECK(fw_an_save(&A, blob, n - 1) == 0);
    n = fw_an_save(&A, blob, sizeof blob);
    fw_an_init(&B);
    CHECK(fw_an_load(&B, blob, n));
    CHECK(fw_an_json(&A, j1, sizeof j1) < sizeof j1);
    CHECK(fw_an_json(&B, j2, sizeof j2) < sizeof j2);
    CHECK_STR(j1, j2);
    CHECK(B.total_sightings == A.total_sightings && B.total_observations == A.total_observations);
    CHECK(memcmp(A.hours, B.hours, sizeof A.hours) == 0);
    CHECK(memcmp(A.heat.cells, B.heat.cells, sizeof A.heat.cells) == 0);
    /* any damage is refused and leaves the state alone */
    for (size_t i = 0; i < n; i += 37) {
        blob[i] ^= 0x10;
        CHECK(!fw_an_load(&B, blob, n));
        blob[i] ^= 0x10;
    }
    CHECK(!fw_an_load(&B, blob, n - 1));
    CHECK(!fw_an_load(&B, NULL, 0));
    fw_an_json(&B, j2, sizeof j2);
    CHECK_STR(j1, j2);
    /* restoring keeps the live dedup table */
    {
        uint32_t k = fw_an_flight_key(NULL, "LIVE", NULL), now = T0 + 2000u * 97;
        fw_an_observe(&B, k, "AAL", now, EDT);
        CHECK(fw_an_load(&B, blob, n));
        CHECK(!fw_an_observe(&B, k, "AAL", now + 60, EDT));
    }
}

int main(void){
    test_keys();
    test_local_time();
    test_dedup();
    test_hours_days_coverage();
    test_top();
    test_types_airports_rare_heat();
    test_json();
    test_checkpoint();
    printf("PASS: %d analytics checks (state %u bytes, checkpoint up to %u)\n", checks, (unsigned)sizeof(FwAnalytics),
           (unsigned)FW_AN_BLOB_MAX);
    return 0;
}
