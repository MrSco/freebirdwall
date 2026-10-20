/* The owned Sync schedule (components/fw_sync/fw_schedule.c). */
#include <stdio.h>
#include <string.h>
#include "fw_schedule.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { ++checks; if (strcmp((a), (b))) { ++failed; printf("FAIL line %d: \"%s\" != \"%s\"\n", __LINE__, (a), (b)); } } while (0)

#define T0 1792500000u                       /* 2026-10-21 10:40 UTC */
#define H 3600u
#define M 60u

static FwSchedule s;

static void leg(const char *al, const char *fn, const char *o, const char *d, uint32_t dep, uint32_t arr, int type, int canceled){
    FwSchedLeg *l = &s.legs[s.count++];
    memset(l, 0, sizeof *l);
    snprintf(l->al, sizeof l->al, "%s", al); snprintf(l->fn, sizeof l->fn, "%s", fn);
    snprintf(l->orig, sizeof l->orig, "%s", o); snprintf(l->dest, sizeof l->dest, "%s", d);
    l->dep_at = dep; l->arr_at = arr; l->type = (uint8_t)type; l->canceled = (uint8_t)canceled;
}

static void test_rel(void){
    char b[24];
    fw_sched_rel(T0 + 2 * H + 5 * M, T0, b, sizeof b);  CHECK_STR(b, "in 2h 05m");
    fw_sched_rel(T0 + 45 * M + 59, T0, b, sizeof b);    CHECK_STR(b, "in 45m");
    fw_sched_rel(T0, T0, b, sizeof b);                  CHECK_STR(b, "in 0m");
    fw_sched_rel(T0 + 3 * 24 * H + 5 * H, T0, b, sizeof b); CHECK_STR(b, "in 3d");
    fw_sched_rel(T0 - 35 * M, T0, b, sizeof b);         CHECK_STR(b, "35m ago");
    fw_sched_rel(T0 - (2 * H + 5 * M), T0, b, sizeof b); CHECK_STR(b, "2h 05m ago");
    fw_sched_rel(T0 - 2 * 24 * H, T0, b, sizeof b);     CHECK_STR(b, "2d ago");
    /* stock's rules: "in " stripped, a digit then 'd' hides the arrival on the next-leg screen */
    fw_sched_rel(T0 + 3 * 24 * H, T0, b, sizeof b);     CHECK_STR(fw_sync_strip_in(b), "3d");
}

static void test_view(void){
    FwSchedView v;
    fw_sched_init(&s);
    s.enabled = true;
    leg("AA", "100", "CLT", "PBI", T0 - H, T0 + H, FW_LEG_OPERATING, 0);          /* in the air */
    leg("AA", "200", "PBI", "CLT", T0 + 3 * H, T0 + 5 * H, FW_LEG_OPERATING, 0);
    leg("AA", "300", "CLT", "DFW", T0 + 24 * H, T0 + 27 * H, FW_LEG_DEADHEAD, 0);
    leg("AA", "400", "DFW", "CLT", T0 + 48 * H, T0 + 51 * H, FW_LEG_OPERATING, 1);
    leg("AA", "000", "", "", T0 + 60 * H, T0 + 70 * H, FW_LEG_RESERVE, 0);
    fw_sched_view(&s, T0, &v);
    CHECK(v.has_active && !strcmp(v.active.fn, "100") && !strcmp(v.active.lt, "operating"));
    CHECK_STR(v.active.dep_rel, "1h 00m ago");
    CHECK_STR(v.active.arr_rel, "in 1h 00m");
    CHECK(v.upcoming_count == 2 && !strcmp(v.upcoming[0].fn, "200") && !strcmp(v.upcoming[1].fn, "400"));   /* deadhead, reserve hidden */
    CHECK(!strcmp(v.upcoming[1].ls, "canceled") && !strcmp(fw_sync_status(&v.upcoming[1]), "Canceled"));
    CHECK(v.message == NULL);
    s.show_deadhead = s.show_reserve = true;
    snprintf(s.message, sizeof s.message, "Home Friday!");
    fw_sched_view(&s, T0, &v);
    CHECK(v.upcoming_count == 4 && !strcmp(v.upcoming[1].lt, "deadhead") && !strcmp(v.upcoming[3].lt, "reserve"));
    CHECK(!strcmp(fw_sync_status(&v.upcoming[1]), "Deadhead") && v.message && !strcmp(v.message, "Home Friday!"));
    /* landed 20 min ago, the next leaves in 1 h 40 m (inside its 2 h window): the landed one first */
    fw_sched_view(&s, T0 + H + 20 * M, &v);
    CHECK(v.has_active && !strcmp(v.active.fn, "100") && !strcmp(v.active.arr_rel, "20m ago"));
    /* 40 min after landing, next one 1 h 20 m away: the next one */
    fw_sched_view(&s, T0 + H + 40 * M, &v);
    CHECK(v.has_active && !strcmp(v.active.fn, "200") && !strcmp(v.active.dep_rel, "in 1h 20m"));
    CHECK(v.upcoming_count == 3 && !strcmp(v.upcoming[0].fn, "300"));       /* the active leg is not listed again */
    /* nothing within 2 h: no active leg, upcoming only */
    fw_sched_view(&s, T0 + 6 * H, &v);
    CHECK(!v.has_active && v.upcoming_count == 3);
    /* a canceled leg never becomes active */
    fw_sched_view(&s, T0 + 48 * H + 10 * M, &v);
    CHECK(!v.has_active && v.upcoming_count == 1 && !strcmp(v.upcoming[0].fn, "000"));
    /* at most five upcoming */
    fw_sched_init(&s); s.enabled = true;
    for (int i = 0; i < 8; ++i) leg("DL", "1", "ATL", "JFK", T0 + (10 + i) * H, T0 + (11 + i) * H, 0, 0);
    fw_sched_view(&s, T0, &v);
    CHECK(!v.has_active && v.upcoming_count == FW_SYNC_UPCOMING_MAX);
    /* switched off: nothing */
    s.enabled = false;
    fw_sched_view(&s, T0, &v);
    CHECK(!v.has_active && v.upcoming_count == 0);
}

static void test_json(void){
    static FwSchedule a, b;
    static char j1[4096], j2[4096];
    char why[80];
    const char *in = "{\"enabled\":true,\"main_view\":\"message\",\"upcoming_view\":\"list\",\"show_deadhead\":true,"
                     "\"message\":\"Love you \\u2708\",\"legs\":["
                     "{\"al\":\"AA\",\"fn\":\"200\",\"orig\":\"PBI\",\"dest\":\"CLT\",\"dep\":\"2026-10-21T15:40Z\",\"arr\":\"2026-10-21T17:40Z\"},"
                     "{\"al\":\"AAL\",\"fn\":\"100A\",\"orig\":\"KCLT\",\"dest\":\"KPBI\",\"dep\":\"2026-10-21 09:40:00-04:00\","
                     "\"arr\":\"2026-10-21T11:40:00-04:00\",\"type\":\"deadhead\",\"canceled\":true}]}";
    CHECK(fw_sched_parse(in, &a, why, sizeof why));
    CHECK(a.enabled && a.message_view && a.list_view && a.show_deadhead && !a.show_reserve && a.show_upcoming);
    CHECK(a.count == 2 && !strcmp(a.legs[0].fn, "100A"));           /* sorted by departure */
    CHECK(a.legs[0].dep_at + 2 * H == a.legs[1].dep_at && a.legs[0].type == FW_LEG_DEADHEAD && a.legs[0].canceled);
    CHECK(!strcmp(a.message, "Love you \xe2\x9c\x88"));
    fw_sched_json(&a, j1, sizeof j1);
    CHECK(fw_sched_parse(j1, &b, why, sizeof why));
    fw_sched_json(&b, j2, sizeof j2);
    CHECK_STR(j1, j2);                                               /* round trip */
    CHECK(strstr(j1, "\"dep\":\"2026-10-21T13:40Z\"") != NULL);         /* 09:40-04:00 in UTC */
    CHECK(strstr(j1, "\"dep\":\"2026-10-21T15:40Z\"") != NULL);
    /* refusals leave the schedule untouched */
    static const char *const bad[][2] = {
        {"[]", "not a JSON object"},
        {"{\"enabled\":1}", "switches must be true or false"},
        {"{\"main_view\":\"big\"}", "main_view: active or message"},
        {"{\"legs\":[{\"al\":\"A\",\"fn\":\"1\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: airline is 2-3 letters/digits"},
        {"{\"legs\":[{\"al\":\"aa\",\"fn\":\"1\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: airline is 2-3 letters/digits"},
        {"{\"legs\":[{\"al\":\"AA\",\"fn\":\"X1\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: flight number like 1234"},
        {"{\"legs\":[{\"al\":\"AA\",\"fn\":\"1\",\"orig\":\"PB\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: airports are 3-4 letter codes"},
        {"{\"legs\":[{\"al\":\"AA\",\"fn\":\"1\",\"dep\":\"soon\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: departure and arrival times"},
        {"{\"legs\":[{\"al\":\"AA\",\"fn\":\"1\",\"dep\":\"2026-10-21T12:00Z\",\"arr\":\"2026-10-21T11:00Z\"}]}", "leg 1: arrival must be after departure, within 24 h"},
        {"{\"legs\":[{\"al\":\"AA\",\"fn\":\"1\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\",\"type\":\"jumpseat\"}]}", "leg 1: type operating, deadhead or reserve"},
        {"{\"message\":\"two\\nlines\"}", "message: one line of text"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        b = a;
        CHECK(!fw_sched_parse(bad[i][0], &b, why, sizeof why));
        CHECK_STR(why, bad[i][1]);
        CHECK(!memcmp(&b, &a, sizeof a));
    }
    {
        static char many[8192];
        size_t n = (size_t)snprintf(many, sizeof many, "{\"legs\":[");
        for (int i = 0; i < 21; ++i)
            n += (size_t)snprintf(many + n, sizeof many - n, "%s{\"al\":\"AA\",\"fn\":\"%d\",\"dep\":\"2026-10-21T10:00Z\",\"arr\":\"2026-10-21T11:00Z\"}", i ? "," : "", i + 1);
        snprintf(many + n, sizeof many - n, "]}");
        CHECK(!fw_sched_parse(many, &b, why, sizeof why));
        CHECK_STR(why, "at most 20 legs");
    }
}

static void test_blob(void){
    static FwSchedule a, b;
    static uint8_t blob[FW_SCHED_BLOB_MAX];
    size_t n;
    fw_sched_init(&a);
    a.enabled = a.list_view = a.show_reserve = true;
    a.show_upcoming = false;
    snprintf(a.message, sizeof a.message, "On my way");
    s = a; s.count = 0;
    for (int i = 0; i < FW_SCHED_MAX; ++i) leg("B6", "1234", "KJFK", "SJU", T0 + i * H, T0 + i * H + 3 * H, i % 3, i % 2);
    a = s;
    n = fw_sched_save(&a, blob, sizeof blob);
    CHECK(n > 0 && n <= FW_SCHED_BLOB_MAX);
    printf("blob: %u bytes for %d legs\n", (unsigned)n, a.count);
    CHECK(fw_sched_load(&b, blob, n));
    CHECK(!memcmp(&a, &b, sizeof a));
    for (size_t i = 0; i < n; i += 37) {                 /* any damaged byte is refused */
        blob[i] ^= 0x5a;
        fw_sched_init(&b);
        CHECK(!fw_sched_load(&b, blob, n));
        blob[i] ^= 0x5a;
    }
    CHECK(!fw_sched_load(&b, blob, n - 1));
    CHECK(fw_sched_save(&a, blob, 100) == 0);            /* too small: nothing written */
}

static void test_callsign(void){
    char cs[16];
    fw_sched_callsign("AA1234", cs, sizeof cs);  CHECK_STR(cs, "AAL1234");
    fw_sched_callsign("DL0012", cs, sizeof cs);  CHECK_STR(cs, "DAL12");
    fw_sched_callsign("B6100", cs, sizeof cs);   CHECK_STR(cs, "JBU100");
    fw_sched_callsign("AAL1234", cs, sizeof cs); CHECK_STR(cs, "AAL1234");   /* already a callsign */
    fw_sched_callsign("ZZ12", cs, sizeof cs);    CHECK_STR(cs, "ZZ12");      /* unknown airline: as typed */
    fw_sched_callsign("AA", cs, sizeof cs);      CHECK_STR(cs, "AA");
    fw_sched_callsign("UA0", cs, sizeof cs);     CHECK_STR(cs, "UAL0");
}

static void test_live_window(void){
    FwSyncLeg l;
    memset(&l, 0, sizeof l);
    l.dep_at = 100000;
    l.arr_at = 107200;                                   /* a 2 h leg */
    CHECK(!fw_sched_live_window(&l, 100000 - 1801));
    CHECK(fw_sched_live_window(&l, 100000 - 1800));      /* 30 min before departure */
    CHECK(fw_sched_live_window(&l, 103000));
    CHECK(fw_sched_live_window(&l, 107200 + 1800));      /* 30 min after arrival */
    CHECK(!fw_sched_live_window(&l, 107200 + 1801));
    strcpy(l.ls, "canceled");
    CHECK(!fw_sched_live_window(&l, 103000));
    l.ls[0] = 0;
    l.arr_at = 0;                                        /* arrival unknown: 6 h */
    CHECK(fw_sched_live_window(&l, 100000 + 6 * 3600 + 1800));
    CHECK(!fw_sched_live_window(&l, 100000 + 6 * 3600 + 1801));
    l.dep_at = 0;
    CHECK(!fw_sched_live_window(&l, 100000));
}

int main(void){
    test_live_window();
    test_callsign();
    test_rel();
    test_view();
    test_json();
    test_blob();
    printf("%s: %d schedule checks\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
