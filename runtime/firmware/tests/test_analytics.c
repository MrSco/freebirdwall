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
    CHECK_STR(top[1].code, "UAL"); CHECK(top[1].count == 5);         /* lower case folded */
    CHECK_STR(top[2].code, "DAL"); CHECK_STR(top[3].code, "SWA");    /* tie: code order */
    CHECK_STR(top[4].code, "JBU");
    CHECK(fw_an_top_airlines(&A, d0 + 1, top) == 0);

    /* more airlines than slots: the frequent ones survive with exact counts */
    fw_an_init(&A);
    n = 0;
    for (int round = 0; round < 50; ++round) {
        char cs[16], code[8];
        snprintf(cs, sizeof cs, "BIG%d", round);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), "BIG", T0 + (uint32_t)n++, 0);
        snprintf(cs, sizeof cs, "R%d", round);
        snprintf(code, sizeof code, "Z%02d", round);                  /* 50 one-off airlines */
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), code, T0 + (uint32_t)n++, 0);
    }
    CHECK(fw_an_top_airlines(&A, d0, top) == FW_AN_TOP);
    CHECK_STR(top[0].code, "BIG"); CHECK(top[0].count == 50 && top[0].error == 0);
    for (int i = 1; i < FW_AN_TOP; ++i) CHECK(top[i].count - top[i].error <= 1);   /* bounds stay honest */
    CHECK(fw_an_day(&A, d0)->sightings == 100);
    /* no airline: counted, not ranked */
    fw_an_observe(&A, fw_an_flight_key(NULL, "NOAL", NULL), "", T0 + 500, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "NULLAL", NULL), NULL, T0 + 501, 0);
    CHECK(fw_an_day(&A, d0)->sightings == 102);
}

static void test_json(void){
    char buf[2048];
    size_t n;
    int32_t h0 = fw_an_local_hour(T0, 0), d0 = fw_an_local_day(T0, 0);
    char expect[256];
    fw_an_init(&A);
    CHECK(fw_an_json(&A, buf, sizeof buf) == strlen("{\"hours\":[],\"days\":[]}"));
    CHECK_STR(buf, "{\"hours\":[],\"days\":[]}");
    fw_an_covered(&A, T0, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "A1", NULL), "AAL", T0, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "A2", NULL), "AAL", T0 + 3600, 0);
    fw_an_observe(&A, fw_an_flight_key(NULL, "D1", NULL), "DAL", T0 + 3601, 0);
    n = fw_an_json(&A, buf, sizeof buf);
    snprintf(expect, sizeof expect, "{\"hours\":[[%ld,1,1],[%ld,2,0]],\"days\":[[%ld,3,1,[[\"AAL\",2],[\"DAL\",1]]]]}",
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
    size_t n;
    char j1[4096], j2[4096];
    fw_an_init(&A);
    for (int i = 0; i < 2000; ++i) {
        char cs[16];
        snprintf(cs, sizeof cs, "F%d", i % 700);
        fw_an_covered(&A, T0 + (uint32_t)i * 97, EDT);
        fw_an_observe(&A, fw_an_flight_key(NULL, cs, NULL), (i % 3) ? "AAL" : "SWA", T0 + (uint32_t)i * 97, EDT);
    }
    n = fw_an_save(&A, blob, sizeof blob);
    CHECK(n > 0 && n <= FW_AN_BLOB_MAX);
    CHECK(fw_an_save(&A, blob, n - 1) == 0);                         /* too small */
    n = fw_an_save(&A, blob, sizeof blob);
    fw_an_init(&B);
    CHECK(fw_an_load(&B, blob, n));
    fw_an_json(&A, j1, sizeof j1);
    fw_an_json(&B, j2, sizeof j2);
    CHECK_STR(j1, j2);
    CHECK(B.total_sightings == A.total_sightings && B.total_observations == A.total_observations);
    CHECK(memcmp(A.hours, B.hours, sizeof A.hours) == 0);
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
    /* restoring keeps the live dedup table: a flight seen a minute ago is still the same sighting */
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
    test_json();
    test_checkpoint();
    printf("PASS: %d analytics checks (state %u bytes, checkpoint up to %u)\n", checks, (unsigned)sizeof(FwAnalytics),
           (unsigned)FW_AN_BLOB_MAX);
    return 0;
}
