/* Desktop tests for components/fw_sync (stock Plus Sync).
 * Run: py tests/run_host_tests.py
 * Expected values are worked by hand from SYNC.md; epochs from Python's
 * datetime. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_sync.h"

static int checks;
static int nan_d(double v){ return v != v; }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_, b_); exit(1); } ++checks; } while (0)

#define W 128
#define H 64
static unsigned char fb[H][W][3];
static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
}
static const FwSurface S = {NULL, W, H, put, NULL};
static int is(int x, int y, int r, int g, int b){ return fb[y][x][0] == r && fb[y][x][1] == g && fb[y][x][2] == b; }
static int count_color(int x0, int y0, int w, int h, int r, int g, int b){
    int n = 0;
    for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) n += is(x, y, r, g, b);
    return n;
}
/* leftmost / topmost lit pixel of a colour in a region */
static void extent(int x0, int y0, int w, int h, int r, int g, int b, int *left, int *top, int *right){
    *left = *top = 9999; *right = -1;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            if (is(x, y, r, g, b)) {
                if (x < *left) *left = x;
                if (y < *top) *top = y;
                if (x > *right) *right = x;
            }
}

static FwSyncLeg leg(const char *al, const char *fn, const char *orig, const char *dest, const char *dep, const char *arr){
    FwSyncLeg l;
    fw_sync_leg_init(&l);
    snprintf(l.lt, sizeof l.lt, "operating");
    snprintf(l.al, sizeof l.al, "%s", al);
    snprintf(l.fn, sizeof l.fn, "%s", fn);
    snprintf(l.orig, sizeof l.orig, "%s", orig);
    snprintf(l.dest, sizeof l.dest, "%s", dest);
    snprintf(l.dep_rel, sizeof l.dep_rel, "%s", dep);
    snprintf(l.arr_rel, sizeof l.arr_rel, "%s", arr);
    return l;
}

static void test_strings(void){
    char b[64];
    FwSyncLeg l = leg("AAL", "1234", "CLT", "PBI", "in 2h 05m", "in 4h 10m");
    fw_sync_flight(&l, b, sizeof b); CHECK_STR(b, "AAL1234");
    fw_sync_route(&l, b, sizeof b); CHECK_STR(b, "CLT-PBI");
    CHECK_STR(fw_sync_status(&l), "Operating");
    l.fn[0] = 0;
    fw_sync_flight(&l, b, sizeof b); CHECK_STR(b, "");
    l.orig[0] = 0;
    fw_sync_route(&l, b, sizeof b); CHECK_STR(b, "----PBI");
    l.dest[0] = 0;
    fw_sync_route(&l, b, sizeof b); CHECK_STR(b, "-------");
    snprintf(l.lt, sizeof l.lt, "deadhead");
    CHECK_STR(fw_sync_status(&l), "Deadhead");
    snprintf(l.ls, sizeof l.ls, "canceled");
    CHECK_STR(fw_sync_status(&l), "Canceled");                  /* canceled wins */
    CHECK(fw_sync_canceled(&l));
    snprintf(l.ls, sizeof l.ls, "Canceled");
    CHECK(!fw_sync_canceled(&l));                               /* exact match only */
    snprintf(l.lt, sizeof l.lt, "reserve");
    CHECK_STR(fw_sync_status(&l), "");
    CHECK_STR(fw_sync_strip_in("in 2h"), "2h");
    CHECK_STR(fw_sync_strip_in("In 2h"), "In 2h");
    CHECK_STR(fw_sync_strip_in("in"), "in");
    CHECK_STR(fw_sync_strip_in("35m ago"), "35m ago");

    fw_sync_fit("ABCDEFGHIJ", 5, b, sizeof b); CHECK_STR(b, "ABCD.");
    fw_sync_fit("ABCDE", 5, b, sizeof b); CHECK_STR(b, "ABCDE");
    fw_sync_fit("ABC DEFG", 5, b, sizeof b); CHECK_STR(b, "ABC.");   /* "ABC " trimmed */
    fw_sync_fit(" ABCDEFG", 5, b, sizeof b); CHECK_STR(b, "ABC.");
    fw_sync_fit("ABC", 0, b, sizeof b); CHECK_STR(b, "");
    fw_sync_fit("Z\xc3\xbcrich-Paris", 4, b, sizeof b); CHECK_STR(b, "Z\xc3\xbcr.");   /* code points */

    fw_sync_wrap("Hello there friend", 10, b, b + 32, 32);
    CHECK_STR(b, "Hello"); CHECK_STR(b + 32, "there friend");
    fw_sync_wrap("  Hi  mom ", 10, b, b + 32, 32);
    CHECK_STR(b, "Hi mom"); CHECK_STR(b + 32, "");
    fw_sync_wrap("Supercalifragilistic is long", 10, b, b + 32, 32);
    CHECK_STR(b, "Supercalifragilistic"); CHECK_STR(b + 32, "is long");   /* first word always on line 1 */
    fw_sync_wrap("abcd efgh ij", 10, b, b + 32, 32);
    CHECK_STR(b, "abcd efgh"); CHECK_STR(b + 32, "ij");                 /* 9 + 1 + 2 > 10 */
    fw_sync_wrap("abcd efgh i", 11, b, b + 32, 32);
    CHECK_STR(b, "abcd efgh i"); CHECK_STR(b + 32, "");                 /* exactly 11 fits */
}

static void test_time(void){
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00Z") == 1792420200u);
    CHECK(fw_sync_parse_time("2026-10-19t14:30z") == 1792420200u);
    CHECK(fw_sync_parse_time("2026-10-19 14:30:00") == 1792420200u);           /* no zone: UTC */
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00.123Z") == 1792420200u);
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00,5+00:00") == 1792420200u);
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00+02:00") == 1792413000u);
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00-0530") == 1792440000u);
    CHECK(fw_sync_parse_time("2026-10-19T14:30:00+05") == 1792402200u);
    CHECK(fw_sync_parse_time("2026-10-19T14:30:60Z") == 1792420259u);          /* leap second -> 59 */
    CHECK(fw_sync_parse_time("2000-02-29T00:00Z") == 951782400u);
    CHECK(fw_sync_parse_time("") == 0);
    CHECK(fw_sync_parse_time(NULL) == 0);
    CHECK(fw_sync_parse_time("2026-10-19") == 0);
    CHECK(fw_sync_parse_time("2026-13-19T14:30Z") == 0);
    CHECK(fw_sync_parse_time("2026-10-19T24:30Z") == 0);
    CHECK(fw_sync_parse_time("2026-10-19T14:30Zjunk") == 0);
    CHECK(fw_sync_parse_time("26-10-19T14:30Z") == 0);
}

static void test_footers_context_progress(void){
    char f1[48], f2[48], ctx[FW_SYNC_CONTEXT_MAX][40];
    FwSyncLeg l = leg("AAL", "1234", "CLT", "PBI", "in 2h 05m", "in 4h 10m");
    FwSyncLive live = {"14:05", "35m", "Charlotte", "320 mi", 42.5};
    int n;

    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f1, "Departs in 2h 05m"); CHECK_STR(f2, "Arrives in 4h 10m");
    snprintf(l.dep_rel, sizeof l.dep_rel, "35m ago");
    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f1, "Departed 35m ago"); CHECK_STR(f2, "Arriving in 4h 10m");
    snprintf(l.arr_rel, sizeof l.arr_rel, "in now");
    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f2, "Arriving now");
    l.arr_rel[0] = 0;
    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f2, "");
    l.dep_rel[0] = 0;
    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f1, ""); CHECK_STR(f2, "");
    snprintf(l.dep_rel, sizeof l.dep_rel, "in 3h");
    snprintf(l.ls, sizeof l.ls, "canceled");
    fw_sync_footers(&l, NULL, f1, f2, sizeof f1);
    CHECK_STR(f1, "Flight canceled"); CHECK_STR(f2, "Scheduled in 3h");
    fw_sync_footers(&l, &live, f1, f2, sizeof f1);              /* live tracking comes first */
    CHECK_STR(f1, "Departed 14:05"); CHECK_STR(f2, "Arriving in 35m");

    fw_sync_message_footers("Running late, see you at dinner tonight", 20, f1, f2, sizeof f1);
    CHECK_STR(f1, "Running late, see"); CHECK_STR(f2, "you at dinner tonig.");

    l = leg("AAL", "1234", "CLT", "PBI", "35m ago", "in 1h 10m");
    n = fw_sync_context(&l, NULL, ctx);
    CHECK(n == 2); CHECK_STR(ctx[0], "CLT-PBI"); CHECK_STR(ctx[1], "Arriving in 1h 10m");
    n = fw_sync_context(&l, &live, ctx);
    CHECK(n == 4); CHECK_STR(ctx[1], "Arriving in 35m"); CHECK_STR(ctx[2], "Over Charlotte"); CHECK_STR(ctx[3], "320 mi flown");
    snprintf(l.ls, sizeof l.ls, "canceled");
    n = fw_sync_context(&l, NULL, ctx);
    CHECK(n == 1);

    l = leg("AAL", "1234", "CLT", "PBI", "", "");
    CHECK(nan_d(fw_sync_progress(&l, NULL, 1000)));
    l.dep_at = 1000; l.arr_at = 2000;
    CHECK(fw_sync_progress(&l, NULL, 999) == 0.0);
    CHECK(fw_sync_progress(&l, NULL, 1000) == 0.0);
    CHECK(fw_sync_progress(&l, NULL, 1250) == 25.0);
    CHECK(fw_sync_progress(&l, NULL, 2000) == 100.0);
    CHECK(fw_sync_progress(&l, &live, 1250) == 42.5);
    live.prog = NAN;
    CHECK(fw_sync_progress(&l, &live, 1250) == 25.0);
    l.arr_at = 1000;
    CHECK(nan_d(fw_sync_progress(&l, NULL, 1250)));
    l.arr_at = 2000;
    snprintf(l.ls, sizeof l.ls, "canceled");
    CHECK(nan_d(fw_sync_progress(&l, NULL, 1250)));

    {
        char b[32];
        l = leg("AAL", "1234", "CLT", "PBI", "", "");
        fw_sync_ident(&l, 0, b, sizeof b); CHECK_STR(b, "AAL1234");
        fw_sync_ident(&l, 1, b, sizeof b); CHECK_STR(b, "Operating");
        l.lt[0] = 0;
        fw_sync_ident(&l, 1, b, sizeof b); CHECK_STR(b, "AAL1234");
        l.al[0] = 0;
        snprintf(l.lt, sizeof l.lt, "deadhead");
        fw_sync_ident(&l, 0, b, sizeof b); CHECK_STR(b, "Deadhead");
    }

    CHECK(fw_sync_phase_ms(FW_SYNC_PHASE_FLIGHTS, 0, false, 3000) == 3000);
    CHECK(fw_sync_phase_ms(FW_SYNC_PHASE_FLIGHTS, 4, false, 5000) == 20000);
    CHECK(fw_sync_phase_ms(FW_SYNC_PHASE_ACTIVE, 9, false, 2000) == 2000);
    CHECK(fw_sync_phase_ms(FW_SYNC_PHASE_ACTIVE, 9, true, 2000) == 4000);
    CHECK(fw_sync_phase_ms(FW_SYNC_PHASE_UPCOMING, 3, false, 3000) == 9000);
}

static void test_message_screen(void){
    FwSyncLeg l = leg("AAL", "1234", "CLT", "PBI", "35m ago", "in 1h 10m");
    int left, top, right;
    l.dep_at = 1000; l.arr_at = 2000;

    /* "Hi mom" fits at scale 2: 6 glyphs x 12 = 72 px, x = 2 + (124-72)/2 = 28, y = 3 */
    fw_screen_sync_message(&S, NULL, &l, NULL, "Hi mom", 0, 1500);
    extent(0, 0, W, 38, 255, 255, 255, &left, &top, &right);
    CHECK(left == 28 && top == 3);
    CHECK(is(28, 3, 255, 255, 255) && is(29, 16, 255, 255, 255));   /* 'H' stem, doubled */
    /* ident "AAL1234": 7 x 6 = 42, x = 2 + 41 = 43, y = 40 */
    extent(0, 38, W, 12, 255, 255, 255, &left, &top, &right);
    CHECK(left == 43 + 0 && top == 40);
    /* context 0 "CLT-PBI": 42 px -> x 43, y 53 */
    extent(0, 50, W, 12, 255, 255, 255, &left, &top, &right);
    CHECK(left == 43 && top == 53);
    /* progress 50% of 124 = 62 px green, rest grey */
    CHECK(is(2, 62, 0, 255, 0) && is(63, 62, 0, 255, 0) && is(64, 62, 66, 65, 66) && is(125, 62, 66, 65, 66));

    /* phase 1: status and the second context line */
    fw_screen_sync_message(&S, NULL, &l, NULL, "Hi mom", 1, 1500);
    extent(0, 38, W, 12, 255, 255, 255, &left, &top, &right);
    CHECK(left == 2 + (124 - 9 * 6) / 2);                            /* "Operating" */
    extent(0, 50, W, 12, 255, 255, 255, &left, &top, &right);
    CHECK(left == 2 + (124 - 18 * 6) / 2);                           /* "Arriving in 1h 10m" */

    /* too long for scale 2 (line 2 "friend tonight" is 14 > 10): scale 1, two lines at y 11 and 21 */
    fw_screen_sync_message(&S, NULL, &l, NULL, "See you friend tonight", 0, 1500);
    extent(0, 0, W, 38, 255, 255, 255, &left, &top, &right);
    CHECK(top == 11);
    CHECK(count_color(0, 21, W, 8, 255, 255, 255) > 0 && count_color(0, 2, W, 9, 255, 255, 255) == 0);
    /* one short line at scale 1 needs a long word: "Supercalifragilistic" (20) -> y 16 */
    fw_screen_sync_message(&S, NULL, &l, NULL, "Supercalifragilistic", 0, 1500);
    extent(0, 0, W, 38, 255, 255, 255, &left, &top, &right);
    CHECK(top == 16 && left == 2 + (124 - 120) / 2);

    /* no times: no progress bar at all */
    l.dep_at = 0;
    fw_screen_sync_message(&S, NULL, &l, NULL, "", 0, 1500);
    CHECK(count_color(0, 62, W, 1, 66, 65, 66) == 0 && count_color(0, 0, W, 38, 255, 255, 255) == 0);
}

static void test_next_and_list(void){
    FwSyncLeg legs[6];
    int left, top, right;
    legs[0] = leg("AAL", "100", "CLT", "PBI", "in 2h 05m", "in 4h 10m");
    legs[1] = leg("DAL", "42", "ATL", "LAX", "in 1d 3h", "in 1d 8h");
    legs[2] = leg("UAL", "7", "SFO", "NRT", "35m ago", "in 9h");
    snprintf(legs[2].lt, sizeof legs[2].lt, "deadhead");
    legs[3] = leg("AAL", "5", "MIA", "JFK", "in 5h", "");
    snprintf(legs[3].ls, sizeof legs[3].ls, "canceled");
    legs[4] = leg("AAL", "6", "JFK", "BOS", "in 6h", "in 7h");
    legs[5] = leg("AAL", "7", "BOS", "DCA", "in 8h", "in 9h");

    fw_screen_sync_next(&S, NULL, legs, 3, 0);
    extent(0, 0, W, 14, 0, 255, 255, &left, &top, &right);
    CHECK(left == 4 && top == 4);                                    /* "UPCOMING 1/3" cyan */
    CHECK(right <= 4 + 12 * 6);
    extent(0, 14, W, 20, 255, 255, 255, &left, &top, &right);
    CHECK(left == 4 && top == 16);                                   /* flight, scale 2 */
    extent(0, 34, W, 18, 255, 255, 255, &left, &top, &right);
    CHECK(left == 4 && top == 34);                                   /* route, scale 2 */
    extent(0, 54, 66, 8, 0, 255, 0, &left, &top, &right);
    CHECK(left == 4 && top == 54);                                   /* "Dep 2h 05m" green */
    extent(66, 54, 62, 8, 0, 255, 255, &left, &top, &right);
    CHECK(left == 68);                       /* "Arr 4h 10m" is 60 px, wider than the 56 px box: not moved */

    fw_screen_sync_next(&S, NULL, legs, 3, 2);                       /* "Arr 9h": 36 px, right-aligned */
    extent(66, 54, 62, 8, 0, 255, 255, &left, &top, &right);
    CHECK(left == 68 + 56 - 36);
    CHECK(count_color(0, 54, 66, 8, 0, 255, 0) > 0);                 /* "Dep 35m ago" (no "in ") */

    fw_screen_sync_next(&S, NULL, legs, 3, 1);                       /* "1d 8h": no Arr */
    CHECK(count_color(66, 54, 62, 8, 0, 255, 255) == 0);
    CHECK(count_color(0, 54, 66, 8, 0, 255, 0) > 0);

    fw_screen_sync_next(&S, NULL, legs, 1, 0);
    extent(0, 0, W, 14, 0, 255, 255, &left, &top, &right);
    CHECK(right <= 4 + 8 * 6);                                       /* just "UPCOMING" */

    fw_screen_sync_list(&S, NULL, legs, 6);                          /* capped at five rows */
    extent(0, 0, W, 10, 0, 255, 255, &left, &top, &right);
    CHECK(left == 2 && top == 2);                                    /* "UPCOMING - NEXT 5" */
    for (int i = 0; i < 5; ++i) {
        int y = 11 + 11 * i;
        CHECK(count_color(2, y, 12, 8, 0, 255, 255) > 0);            /* index */
        CHECK(count_color(14, y, 44, 8, 255, 255, 255) > 0);         /* route */
    }
    CHECK(count_color(0, 63, W, 1, 255, 255, 255) == 0);
    extent(58, 11, 54, 8, 0, 255, 0, &left, &top, &right);
    CHECK(left == 59 && top == 11);           /* dep_rel raw "in 2h 05m" at x 58 (the i glyph starts one column in) */
    CHECK(count_color(58, 22, 54, 8, 0, 255, 0) > 0);
    /* flags: deadhead "DH" in row 3, canceled "X" in row 4, right-aligned at 126 */
    extent(112, 33, 16, 8, 132, 130, 132, &left, &top, &right);
    CHECK(left == 114 && right <= 125);
    extent(112, 44, 16, 8, 132, 130, 132, &left, &top, &right);
    CHECK(left >= 120 && right <= 125);
    CHECK(count_color(112, 11, 16, 8, 132, 130, 132) == 0);

    fw_screen_sync_list(&S, NULL, legs, 1);
    extent(0, 0, W, 10, 0, 255, 255, &left, &top, &right);
    CHECK(right <= 2 + 8 * 6);                                       /* "UPCOMING" */
}

static void test_active_screen(void){
    static unsigned char page0[H][W][3];
    FwFlightSettings st;
    FwSyncLeg l = leg("AAL", "1234", "CLT", "PBI", "35m ago", "in 1h 10m");
    l.dep_at = 1000; l.arr_at = 2000;
    memset(&st, 0, sizeof st);
    fw_units_parse(&st.units, NULL, NULL, NULL, NULL, NULL);
    st.layout = "DEFAULT";
    st.mini = true;
    /* half way: 62 of 124 px green, the rest grey, as the tracked bar */
    memset(fb, 0x55, sizeof fb);
    fw_screen_sync_active(&S, &st, &l, NULL, NULL, "Hi mom", 0, 0, 1500, NULL);
    CHECK(is(2, 62, 0, 255, 0) && is(63, 62, 0, 255, 0) && is(64, 62, 66, 65, 66));
    CHECK(is(0, 0, 0, 0, 0));                                   /* cleared */
    CHECK(count_color(0, 0, W, 60, 255, 255, 255) > 50);        /* headings and the leg footers */
    memcpy(page0, fb, sizeof fb);
    /* page 4: the message instead of the leg footers */
    fw_screen_sync_active(&S, &st, &l, NULL, NULL, "Hi mom", 4, 0, 1500, NULL);
    CHECK(memcmp(page0, fb, sizeof fb) != 0);
    /* odd phase: the status replaces the flight in heading1 */
    fw_screen_sync_active(&S, &st, &l, NULL, NULL, "Hi mom", 0, 1, 1500, NULL);
    CHECK(memcmp(page0, fb, sizeof fb) != 0);
    /* canceled: no progress, a black line where the bar was */
    snprintf(l.ls, sizeof l.ls, "canceled");
    fw_screen_sync_active(&S, &st, &l, NULL, NULL, NULL, 0, 0, 1500, NULL);
    CHECK(count_color(0, 62, W, 1, 0, 0, 0) == W);
}

int main(void){
    test_strings();
    test_time();
    test_footers_context_progress();
    test_message_screen();
    test_next_and_list();
    test_active_screen();
    printf("PASS: %d sync checks\n", checks);
    return 0;
}
