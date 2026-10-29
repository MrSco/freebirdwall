/* Desktop tests for components/fw_clock. Run: py tests/run_host_tests.py
 * Expected values come from the vendor formulas in CLOCK.md, worked by hand;
 * tests/test_clock.py checks local time against the tz database. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_clock.h"

#define W 128
#define H 64
static unsigned char fb[H][W][3];
static int writes, checks, wrote_in[H][W];

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); exit(1); } ++checks; } while (0)

static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    if (x < 0 || x >= W || y < 0 || y >= H) { fprintf(stderr, "out of bounds pixel %d,%d\n", x, y); exit(1); }
    fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
    ++writes;
    wrote_in[y][x] = 1;
}
static const FwSurface S = {NULL, W, H, put, NULL};

static void reset_marks(void){ writes = 0; memset(wrote_in, 0, sizeof wrote_in); }

static int region_written(int x, int y, int w, int h){
    for (int r = y; r < y + h; ++r)
        for (int c = x; c < x + w; ++c)
            if (wrote_in[r][c]) return 1;
    return 0;
}

/* 2026-10-19 14:25:07 UTC: Monday, 10:25:07 EDT in the owner's zone. */
#define T_OWNER 1792419907LL

static FwClockConfig owner(void){
    FwClockConfig c;
    memset(&c, 0, sizeof c);
    c.zone.standard_offset_min = -300;
    c.zone.dst_rule = FW_DST_NORTH_AMERICA;
    c.large = true;
    c.show_date = true;
    c.day_first = true;
    c.show_weekday = true;
    c.locale = "en";
    return c;
}

static void check_line(const FwClockLine *l, int kind, const char *text, int x, int y, int w, int h){
    CHECK(l->kind == kind);
    CHECK_STR(l->text, text);
    if (l->x != x || l->y != y || l->w != w || l->h != h)
        fprintf(stderr, "'%s' at %d,%d %dx%d\n", l->text, l->x, l->y, l->w, l->h);
    CHECK(l->x == x && l->y == y && l->w == w && l->h == h);
}

static void time_text(FwClockConfig c, int64_t utc, const char *want){
    FwTime t;
    char buf[32];
    CHECK(fw_local_time(utc, c.zone, &t));
    fw_clock_time_text(&c, &t, buf, sizeof buf);
    CHECK_STR(buf, want);
}

static void date_text(FwClockConfig c, const char *want){
    FwTime t;
    char buf[32];
    CHECK(fw_local_time(T_OWNER, c.zone, &t));
    fw_clock_date_text(&c, &t, buf, sizeof buf);
    CHECK_STR(buf, want);
}

static void zone_name(int minutes, int rule, bool dst, const char *want){
    FwZone z = {(int16_t)minutes, (uint8_t)rule};
    char buf[16];
    fw_zone_name(z, dst, buf, sizeof buf);
    CHECK_STR(buf, want);
}

int main(void){
    FwClockConfig c;
    FwClockFrame f, prev;

    /* --- local time ------------------------------------------------------ */
    {
        FwTime t;
        c = owner();
        CHECK(fw_local_time(T_OWNER, c.zone, &t));
        CHECK(t.year == 126 && t.mon == 9 && t.mday == 19 && t.wday == 1);
        CHECK(t.hour == 10 && t.min == 25 && t.sec == 7 && t.dst);
        c.zone.dst_rule = FW_DST_NONE;
        CHECK(fw_local_time(T_OWNER, c.zone, &t) && t.hour == 9 && !t.dst);
        c.zone.standard_offset_min = FW_ZONE_UNSET;   /* unset: UTC, even with a rule */
        c.zone.dst_rule = FW_DST_NORTH_AMERICA;
        CHECK(fw_local_time(T_OWNER, c.zone, &t) && t.hour == 14 && !t.dst);
    }

    /* --- time text ------------------------------------------------------- */
    c = owner();
    time_text(c, T_OWNER, "10:25 AM");
    time_text(c, 1792382700LL, "12:05 AM");   /* 00:05 */
    time_text(c, 1792425600LL, "12:00 PM");   /* 12:00 */
    time_text(c, 1792418399LL, "9:59 AM");
    c.show_seconds = true;
    time_text(c, T_OWNER, "10:25:07 AM");
    time_text(c, 1792418399LL, "9:59:59 AM");
    c.is_24h = true;
    time_text(c, T_OWNER, "10:25:07");
    time_text(c, 1792382700LL, "00:05:00");
    c.show_seconds = false;
    time_text(c, 1792425600LL, "12:00");

    /* --- date text ------------------------------------------------------- */
    c = owner();
    date_text(c, "MON 19 OCT");
    c.day_first = false;
    date_text(c, "MON OCT 19");
    c.show_weekday = false;
    date_text(c, "OCT 19");
    c = owner();
    c.locale = "de";
    date_text(c, "MO 19 OKT");
    c.locale = "fr";
    date_text(c, "LUN 19 OCT");
    c.locale = "ru";
    date_text(c, "\xD0\x9F\xD0\x9D 19 \xD0\x9E\xD0\x9A\xD0\xA2");   /* ПН 19 ОКТ */
    c.locale = "xx";
    date_text(c, "MON 19 OCT");                                   /* unknown -> English */
    c.locale = NULL;
    date_text(c, "MON 19 OCT");

    /* --- zone names ------------------------------------------------------ */
    zone_name(-300, FW_DST_NONE, false, "EST");
    zone_name(-360, FW_DST_NONE, false, "CST");
    zone_name(480, FW_DST_NONE, false, "CST");
    zone_name(0, FW_DST_NONE, false, "GMT");
    zone_name(330, FW_DST_NONE, false, "IST");
    zone_name(345, FW_DST_NONE, false, "UTC+5:45");
    /* owned: while daylight saving is on, the daylight name (stock: EST all year) */
    zone_name(-300, FW_DST_NORTH_AMERICA, false, "EST");
    zone_name(-300, FW_DST_NORTH_AMERICA, true, "EDT");
    zone_name(-480, FW_DST_NORTH_AMERICA, true, "PDT");
    zone_name(-540, FW_DST_NORTH_AMERICA, true, "AKDT");
    zone_name(0, FW_DST_EUROPE, true, "BST");
    zone_name(60, FW_DST_EUROPE, true, "CEST");
    zone_name(120, FW_DST_EUROPE, false, "EET");
    zone_name(-210, FW_DST_NORTH_AMERICA, true, "UTC-2:30");   /* no daylight name: the offset now */
    zone_name(600, FW_DST_EUROPE, true, "UTC+11");             /* a rule the name does not go with */
    zone_name(-210, FW_DST_NONE, false, "UTC-3:30");
    zone_name(-720, FW_DST_NONE, false, "UTC-12");
    zone_name(780, FW_DST_NONE, false, "UTC+13");
    zone_name(FW_ZONE_UNSET, FW_DST_NORTH_AMERICA, true, "UTC");

    /* --- layout: the owner's LARGE clock ----------------------------------- */
    c = owner();
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 2 && f.scale == 2);
    check_line(&f.lines[0], FW_CLOCK_DATE, "MON 19 OCT", 4, 15, 120, 16);
    check_line(&f.lines[1], FW_CLOCK_TIME, "10:25 AM", 16, 32, 96, 16);

    /* LARGE falls back to 1x when a line is too wide for 2x */
    c.show_seconds = true;                               /* "10:25:07 AM": 11 x 12 = 132 > 128 */
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 2 && f.scale == 1);
    check_line(&f.lines[0], FW_CLOCK_DATE, "MON 19 OCT", 34, 23, 60, 8);
    check_line(&f.lines[1], FW_CLOCK_TIME, "10:25:07 AM", 31, 32, 66, 8);

    /* DEFAULT style: 1x, centred */
    c = owner();
    c.large = false;
    c.show_date = false;
    c.is_24h = true;
    c.show_seconds = true;
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 1 && f.scale == 1);
    check_line(&f.lines[0], FW_CLOCK_TIME, "10:25:07", 40, 28, 48, 8);

    /* extra zones: three lines at most, date first; labels trimmed and upper-cased */
    c = owner();
    c.large = false;
    c.extra_count = 2;
    strcpy(c.extra[0].label, "  london ");
    c.extra[0].zone.standard_offset_min = 0;
    c.extra[0].zone.dst_rule = FW_DST_EUROPE;
    c.extra[1].label[0] = 0;
    c.extra[1].zone.standard_offset_min = 540;
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 3 && f.scale == 1);
    check_line(&f.lines[0], FW_CLOCK_DATE, "MON 19 OCT", 34, 19, 60, 8);
    check_line(&f.lines[1], FW_CLOCK_TIME, "EDT 10:25 AM", 28, 28, 72, 8);  /* daylight saving on October 19 */
    check_line(&f.lines[2], FW_CLOCK_ZONE, "LONDON 3:25 PM", 22, 37, 84, 8);
    c.show_date = false;                                 /* no date: both extras */
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 3);
    CHECK_STR(f.lines[2].text, "JST 11:25 PM");
    c.extra[1].zone.standard_offset_min = 60;            /* no label: the zone's name, daylight in October */
    c.extra[1].zone.dst_rule = FW_DST_EUROPE;
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK_STR(f.lines[2].text, "CEST 4:25 PM");
    fw_clock_frame(&c, T_OWNER + 30LL * 86400, W, H, &f);   /* November 18: both back on standard time */
    CHECK_STR(f.lines[0].text, "EST 9:25 AM");
    CHECK_STR(f.lines[1].text, "LONDON 2:25 PM");
    CHECK_STR(f.lines[2].text, "CET 3:25 PM");
    c.extra[1].zone.standard_offset_min = 540;
    c.extra[1].zone.dst_rule = FW_DST_NONE;
    c.large = true;                                      /* LARGE: two lines, so one extra */
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 2 && f.scale == 1);                 /* 14 x 12 = 168 > 128 */
    c.show_date = true;                                  /* LARGE with date: no extras */
    fw_clock_frame(&c, T_OWNER, W, H, &f);
    CHECK(f.count == 2 && f.scale == 2);
    CHECK_STR(f.lines[1].text, "10:25 AM");

    /* --- drawing ---------------------------------------------------------- */
    {
        /* the owner's clock: every pixel matches plain 2x text at the laid-out spots */
        static unsigned char want[H][W][3];
        c = owner();
        memset(fb, 0x55, sizeof fb);
        fw_screen_clock(&S, &c, T_OWNER);
        memcpy(want, fb, sizeof fb);
        memset(fb, 0, sizeof fb);
        fw_text_draw(&S, 4, 15, "MON 19 OCT", (FwRgb){255, 255, 255}, 2);
        fw_text_draw(&S, 16, 32, "10:25 AM", (FwRgb){255, 255, 255}, 2);
        CHECK(memcmp(want, fb, sizeof fb) == 0);
    }
    {
        /* a Cyrillic date uses the clock's own glyphs: П drawn from its rows */
        static const char *const pe[] = {"#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", "....."};
        c = owner();
        c.large = false;
        c.locale = "ru";
        fw_clock_frame(&c, T_OWNER, W, H, &f);
        memset(fb, 0, sizeof fb);
        fw_clock_draw(&S, &f, NULL);
        for (int r = 0; r < 8; ++r)
            for (int k = 0; k < 6; ++k) {
                int lit = k < 5 && pe[r][k] == '#';
                CHECK(fb[f.lines[0].y + r][f.lines[0].x + k][0] == (lit ? 255 : 0));
            }
    }

    /* --- redraw: clear only on layout change, otherwise changed lines only - */
    {
        static unsigned char fresh[H][W][3];
        c = owner();
        c.large = false;
        c.show_seconds = true;
        memset(&prev, 0, sizeof prev);
        fw_clock_frame(&c, T_OWNER, W, H, &f);
        reset_marks();
        fw_clock_draw(&S, &f, &prev);
        CHECK(region_written(0, 0, W, H) && wrote_in[0][0]);       /* first frame clears */
        fw_clock_frame(&c, T_OWNER + 1, W, H, &f);                  /* 10:25:08: same layout */
        reset_marks();
        fw_clock_draw(&S, &f, &prev);
        CHECK(!wrote_in[0][0]);                                      /* no clear */
        CHECK(!region_written(f.lines[0].x, f.lines[0].y, f.lines[0].w, f.lines[0].h));  /* date untouched */
        CHECK(region_written(f.lines[1].x, f.lines[1].y, f.lines[1].w, f.lines[1].h));
        memcpy(fresh, fb, sizeof fb);                                 /* opaque redraw == fresh render */
        memset(fb, 0x33, sizeof fb);
        fw_screen_clock(&S, &c, T_OWNER + 1);
        CHECK(memcmp(fresh, fb, sizeof fb) == 0);
        fw_clock_frame(&c, 1792418399LL, W, H, &f);                  /* 9:59:59 AM: narrower, so clear */
        reset_marks();
        fw_clock_draw(&S, &f, &prev);
        CHECK(wrote_in[0][0]);
        reset_marks();
        fw_clock_draw(&S, &f, &prev);                                /* nothing changed: nothing drawn */
        CHECK(writes == 0);
    }

    printf("PASS: %d clock checks\n", checks);
    return 0;
}
