/* Desktop tests for components/fw_flight and the flight screens.
 * Run: py tests/run_host_tests.py
 * Expected strings are worked by hand from the vendor rules in FLIGHT-SCREENS.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_flight.h"
#include "fw_screens.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_, b_); exit(1); } ++checks; } while (0)

static FwFlightSettings settings(void){
    FwFlightSettings s;
    memset(&s, 0, sizeof s);
    fw_units_parse(&s.units, NULL, NULL, NULL, NULL, NULL);
    s.title = FW_TITLE_AIRLINE;
    s.mini = true;
    s.layout = "DEFAULT";
    return s;
}

static FwFlight sample(void){
    FwFlight f;
    fw_flight_init(&f);
    strcpy(f.cs, "AAL1234");
    strcpy(f.reg, "N915NN");
    strcpy(f.al_code, "AAL");
    strcpy(f.al_name, "American");
    strcpy(f.orig_iata, "CLT");
    strcpy(f.dest_iata, "PBI");
    strcpy(f.orig_icao, "KCLT");
    strcpy(f.dest_icao, "KPBI");
    strcpy(f.orig, "Charlotte Douglas");
    strcpy(f.dest, "Palm Beach Intl");
    strcpy(f.ac_name, "737-800");
    return f;
}

static char out[FW_FLIGHT_TEXT_MAX];

static const char *expand(const FwFlightSettings *s, const FwFlight *f, const char *tmpl){
    fw_flight_expand(s, f, tmpl, out, sizeof out);
    return out;
}

static const char *alt(const char *unit, double m){
    FwFlightSettings s = settings();
    FwFlight f = sample();
    fw_units_parse(&s.units, unit, NULL, NULL, NULL, NULL);
    f.alt_b = m;
    return expand(&s, &f, "{alt_b}");
}

static const char *vel(const char *unit, double mps){
    FwFlightSettings s = settings();
    FwFlight f = sample();
    fw_units_parse(&s.units, NULL, unit, NULL, NULL, NULL);
    f.vel = mps;
    return expand(&s, &f, "{vel}");
}

static const char *vr(const char *unit, double mps){
    FwFlightSettings s = settings();
    FwFlight f = sample();
    fw_units_parse(&s.units, NULL, NULL, unit, NULL, NULL);
    f.vr = mps;
    return expand(&s, &f, "{vr}");
}

static const char *trk(const char *unit, double deg){
    FwFlightSettings s = settings();
    FwFlight f = sample();
    fw_units_parse(&s.units, NULL, NULL, NULL, unit, NULL);
    f.trk = deg;
    return expand(&s, &f, "{trk}");
}

static const char *dist(const char *unit, double m){
    FwFlightSettings s = settings();
    FwFlight f = sample();
    fw_units_parse(&s.units, NULL, NULL, NULL, NULL, unit);
    f.has_trk = true;
    f.dist = m;
    return expand(&s, &f, "{trk_dist}");
}

/* ------------------------------------------------------------ drawing */
#define W 128
#define H 64
static unsigned char fb[H][W][3];
static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    if (x < 0 || x >= W || y < 0 || y >= H) { fprintf(stderr, "out of bounds %d,%d\n", x, y); exit(1); }
    fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
}
static const FwSurface S = {NULL, W, H, put, NULL};
static void fill(int r, int g, int b){
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { fb[y][x][0] = (unsigned char)r; fb[y][x][1] = (unsigned char)g; fb[y][x][2] = (unsigned char)b; }
}
static int is(int x, int y, int r, int g, int b){ return fb[y][x][0] == r && fb[y][x][1] == g && fb[y][x][2] == b; }

/* Owned airport aliases: a code alias and a name alias. */
static int alias_calls;
static bool test_alias(void *ctx, const char *token, const char *value, char *out, size_t cap){
    (void)ctx;
    ++alias_calls;
    if (!strcmp(value, "DJT") && strstr(token, "_iata")) { snprintf(out, cap, "PBI"); return true; }
    if (!strcmp(value, "Donald J Trump Intl") && (!strcmp(token, "{orig}") || !strcmp(token, "{dest}"))) {
        snprintf(out, cap, "Palm Beach Intl");
        return true;
    }
    return false;
}

int main(void){
    FwFlightSettings s;
    FwFlight f;
    FwTrackState t;

    /* --- units ------------------------------------------------------------- */
    CHECK_STR(alt("feet", 10668), "35kft");          /* 34,999.99 ft -> 35000 */
    CHECK_STR(alt("feet", 1524), "5.0kft");
    CHECK_STR(alt("feet", 300), "984ft");
    CHECK_STR(alt("nautical_miles", 10668), "6NM");
    CHECK_STR(alt("kilometers", 10668), "11km");
    CHECK_STR(alt("meters", 10668), "10km");         /* 10668 -> "10k" + "m": stock's own reading */
    CHECK_STR(alt("meters", 950), "950m");
    CHECK_STR(alt("feet", -30), "-98ft");
    CHECK_STR(alt("feet", NAN), "N/A");
    CHECK_STR(alt("feet", INFINITY), "N/A");
    CHECK_STR(vel("mph", 231.5), "518mph");
    CHECK_STR(vel("knots", 231.5), "450kts");
    CHECK_STR(vel("kmh", 231.5), "833kph");
    CHECK_STR(vel(NULL, 231.5), "833kph");            /* stock default */
    CHECK_STR(vel("mps", 231.5), "232m/s");
    CHECK_STR(vel("mach", 231.5), "M0.67");
    CHECK_STR(vr("fpm", 10.16), "+2.0kfpm");
    CHECK_STR(vr("mph", 10.16), "+23mph");
    CHECK_STR(vr("kmh", -5), "-18kph");
    CHECK_STR(vr("kmh", 0.05), "0kph");
    CHECK_STR(vr("fps", -0.2), "-1ft/s");
    CHECK_STR(trk("cardinal", 359.6), "N");
    CHECK_STR(trk("cardinal", 45), "NE");
    CHECK_STR(trk("cardinal", 10), "N");
    CHECK_STR(trk("cardinal", 11), "NNE");            /* stock divides by 22, not 22.5 */
    CHECK_STR(trk("cardinal", -90), "W");
    CHECK_STR(trk("cardinal", 337), "NNW");
    CHECK_STR(trk("cardinal", 348), "N");
    CHECK_STR(trk("degrees", 359.6), "0deg");
    CHECK_STR(trk(NULL, 271.4), "271deg");
    CHECK_STR(dist("miles", 160934), "100.0mi");      /* 99.99996 is below 100: one decimal */
    CHECK_STR(dist("miles", 500000), "311mi");
    CHECK_STR(dist("miles", 1000), "0.6mi");
    CHECK_STR(dist("kilometers", 1500), "1.5km");
    CHECK_STR(dist("nautical_miles", 370400), "200NM");
    CHECK_STR(dist("feet", 1000), "3281ft");
    CHECK_STR(dist("meters", 1000), "1000m");
    CHECK_STR(dist("miles", NAN), "N/A");

    /* --- names and text ----------------------------------------------------- */
    s = settings();
    f = sample();
    CHECK_STR(expand(&s, &f, "{al_name}"), "American");
    f.al_name[0] = 0;
    CHECK_STR(expand(&s, &f, "{al_name}"), "AAL1234");
    f.cs[0] = 0;
    CHECK_STR(expand(&s, &f, "{al_name}"), "AAL");
    f.al_code[0] = 0;
    CHECK_STR(expand(&s, &f, "{al_name}"), "N/A");
    f = sample();
    s.title = FW_TITLE_REGISTRATION;
    CHECK_STR(expand(&s, &f, "{al_name}"), "N915NN");
    f.reg[0] = 0;
    CHECK_STR(expand(&s, &f, "{al_name}"), "AAL1234");
    s.title = FW_TITLE_CALLSIGN;
    CHECK_STR(expand(&s, &f, "{al_name} {cs} {reg}"), "AAL1234 AAL1234 AAL1234");
    CHECK(fw_title_parse("registration") == FW_TITLE_REGISTRATION && fw_title_parse("airline") == FW_TITLE_AIRLINE);

    s = settings();
    f = sample();
    strcpy(f.orig, "A\xC3\xA9roport \xE2\x80\x9C" "CDG\xE2\x80\x9D \xE2\x80\x93 Paris\xE2\x80\xA6");
    CHECK_STR(expand(&s, &f, "{orig}"), "A\xC3\xA9roport \"CDG\" - Paris...");
    strcpy(f.ac_name, "Boeing\xC2\xA0" "737");
    CHECK_STR(expand(&s, &f, "{ac_name}"), "Boeing 737");
    strcpy(f.reg, "N\xE2\x80\x93" "1");                 /* registration is not cleaned */
    CHECK_STR(expand(&s, &f, "{reg}"), "N\xE2\x80\x93" "1");

    f = sample();
    {
        static const char *const home_orig[] = {"kclt"}, *const home_dest[] = {"PBI"};
        s.anchors = home_orig; s.anchor_count = 1;
        CHECK_STR(expand(&s, &f, "{orig}|{dest}"), "Departing to|Palm Beach Intl");
        s.anchors = home_dest;
        CHECK_STR(expand(&s, &f, "{orig}|{dest}"), "Arriving from|Charlotte Douglas");
        s.anchors = NULL; s.anchor_count = 0;
    }
    f.orig[0] = f.dest[0] = 0;
    CHECK_STR(expand(&s, &f, "{orig}|{dest}"), "Unknown origin|Unknown destination");
    s.layout = "TECHNICAL";
    CHECK_STR(expand(&s, &f, "{orig}|{dest}"), "N/A|N/A");
    s = settings();
    f = sample();
    f.orig_iata[0] = 0;
    CHECK_STR(expand(&s, &f, "{orig_iata}-{dest_iata}"), "N/A-PBI");
    CHECK_STR(expand(&s, &f, "{sqk} {trk_prog} {trk_dep} [{trk_tft}]"), "N/A N/A N/A []");
    CHECK_STR(expand(&s, &f, "{cs}{cs}{nope}"), "AAL1234AAL1234{nope}");
    strcpy(f.al_name, "{cs}");                           /* inserted text is not rescanned for later tokens */
    CHECK_STR(expand(&s, &f, "{al_name}"), "AAL1234");   /* ...but {cs} comes after {al_name} in stock order */
    fw_flight_expand(&s, &f, NULL, out, sizeof out);
    CHECK_STR(out, "");

    /* --- element templates -------------------------------------------------- */
    CHECK_STR(fw_flight_element_template(true, "DEFAULT", "heading2"), "{orig_iata}-{dest_iata}");
    CHECK_STR(fw_flight_element_template(true, "NO_LOGO", "footer2"), "Alt:{alt_b},Spd:{vel}");
    CHECK_STR(fw_flight_element_template(true, "TECHNICAL", "footer2"), "Trk:{trk},Vr:{vr}");
    CHECK(fw_flight_element_template(true, "PLUS_SYNC_ACTIVE_MAIN", "footer1") == NULL);
    CHECK_STR(fw_flight_element_template(false, "LARGE_LOGO", "tag3"), "Trk: {trk}");
    CHECK(fw_flight_element_template(true, "BOGUS", "heading1") == NULL);
    CHECK(fw_flight_element_template(true, "DEFAULT", "tag1") == NULL);

    /* --- ordinary card text -------------------------------------------------- */
    s = settings();
    f = sample();
    CHECK(fw_flight_text(&s, &f, "footer1", 124, out, sizeof out));
    CHECK_STR(out, "Charlotte Douglas");
    CHECK(!fw_flight_text(&s, &f, "logo", 36, out, sizeof out));
    s.title = FW_TITLE_REGISTRATION;
    strcpy(f.reg, "N123456789ABCDE");                     /* 15 x 6 = 90 px > 86: callsign instead */
    CHECK(fw_flight_text(&s, &f, "heading1", 86, out, sizeof out));
    CHECK_STR(out, "AAL1234");
    CHECK(fw_flight_text(&s, &f, "heading1", 90, out, sizeof out));
    CHECK_STR(out, "N123456789ABCDE");
    s = settings();
    strcpy(f.orig, "Dallas Fort Worth International");     /* 31 bytes, the record's most */
    CHECK(fw_flight_text(&s, &f, "footer1", 124, out, sizeof out));
    CHECK_STR(out, "Dallas Fort Worth");          /* owned: 186 px > 124; "...Intl" is 132 px, so without it */
    CHECK(fw_flight_text(&s, &f, "footer1", 400, out, sizeof out));
    CHECK_STR(out, "Dallas Fort Worth International");      /* fits: as stock */
    s.mini = false;
    CHECK(fw_flight_text(&s, &f, "footer1", 400, out, sizeof out));
    CHECK_STR(out, "Dallas Fort Worth International");      /* widescreen: only over 32 bytes gets ... */
    f = sample();

    /* --- owned: long names shortened only when they do not fit ---------------- */
    s = settings();
    f = sample();
    strcpy(f.al_name, "JetBlue Airways");                   /* 90 px > 86 */
    CHECK(fw_flight_text(&s, &f, "heading1", 86, out, sizeof out));
    CHECK_STR(out, "JetBlue");
    CHECK(fw_flight_text(&s, &f, "heading1", 124, out, sizeof out));
    CHECK_STR(out, "JetBlue Airways");                      /* fits: kept */
    strcpy(f.al_name, "Delta Air Lines");
    CHECK(fw_flight_text(&s, &f, "heading1", 86, out, sizeof out));
    CHECK_STR(out, "Delta");
    strcpy(f.al_name, "British Airways");                   /* would be a lone "British": kept, it scrolls */
    CHECK(fw_flight_text(&s, &f, "heading1", 86, out, sizeof out));
    CHECK_STR(out, "British Airways");
    strcpy(f.al_name, "Southwest Airlines");
    s.title = FW_TITLE_CALLSIGN;                            /* not the airline name: untouched */
    CHECK(fw_flight_text(&s, &f, "heading1", 86, out, sizeof out));
    CHECK_STR(out, f.cs);
    s = settings();
    strcpy(f.orig, "Fort Lauderdale Hollywood Intl");      /* as the community source names it */
    CHECK(fw_flight_text(&s, &f, "footer1", 124, out, sizeof out));
    CHECK_STR(out, "Fort Lauderdale Hollywood");            /* still 150 px: it scrolls */
    strcpy(f.orig, "Raleigh Durham International");
    CHECK(fw_flight_text(&s, &f, "footer1", 124, out, sizeof out));
    CHECK_STR(out, "Raleigh Durham Intl");                  /* level 1 is enough */
    strcpy(f.orig, "Asheville Regional Airport");
    CHECK(fw_flight_text(&s, &f, "footer1", 124, out, sizeof out));
    CHECK_STR(out, "Asheville Rgnl");                       /* 156 px: level 1 fits */
    {
        char t[64];
        strcpy(t, "Asheville Regional Airport");
        fw_flight_short_airport(t, sizeof t, 1);
        CHECK_STR(t, "Asheville Rgnl");
        strcpy(t, "Asheville Rgnl");
        fw_flight_short_airport(t, sizeof t, 2);
        CHECK_STR(t, "Asheville");
        strcpy(t, "Intl");
        fw_flight_short_airport(t, sizeof t, 2);
        CHECK_STR(t, "Intl");                               /* never empty */
        strcpy(t, "JETBLUE AIRWAYS");
        fw_flight_short_airline(t, sizeof t);
        CHECK_STR(t, "JETBLUE");
        strcpy(t, "Airways");
        fw_flight_short_airline(t, sizeof t);
        CHECK_STR(t, "Airways");
    }

    /* --- tracked state and pages --------------------------------------------- */
    s = settings();
    f = sample();
    f.has_trk = true;
    f.prog = 42;
    strcpy(f.dep, "35m ago");
    strcpy(f.arr, "1h 10m");
    strcpy(f.over, "Virginia");
    f.dist = 482803;
    fw_track_state(&f, &t);
    CHECK(!t.landed && !t.on_ground && !t.took);
    {
        uint8_t pages[FW_TRACK_MAX_PAGES];
        CHECK(fw_track_pages(&f, false, pages) == 3 && pages[0] == 0 && pages[1] == 1 && pages[2] == 3);
        CHECK(fw_track_pages(&f, true, pages) == 3);                 /* metrics on, but no metric known */
        f.vel = 231.5;
        CHECK(fw_track_pages(&f, true, pages) == 4 && pages[2] == 2 && pages[3] == 3);
        CHECK(fw_track_page_ms(4, 1) == 5000);
        CHECK(fw_track_page_ms(4, 2) == 2500);
        CHECK(fw_track_page_ms(2, 3) == 5000);
        CHECK(fw_track_page_ms(12, 2) == 1000);
    }
#define TT(page, el, want) do { CHECK(fw_track_text(&s, &f, &t, (page), (el), 124, 1000, out, sizeof out)); \
                                CHECK_STR(out, (want)); } while (0)
    TT(0, "heading1", "AAL1234");
    TT(0, "heading2", "CLT-PBI");
    TT(0, "heading3", "737-800");
    TT(0, "footer1", "Departed 35m ago");
    TT(0, "footer2", "Arriving in 1h 10m");
    TT(1, "footer1", "Flying over");
    TT(1, "footer2", "Virginia");
    s.units.alt = FW_ALT_FEET; s.units.vel = FW_VEL_MPH;
    f.alt_b = 10668;
    TT(2, "footer1", "Alt:35kft,Spd:518mph");
    TT(2, "footer2", "Trk:N/A,Vr:N/A");
    s.units.dist = FW_DIST_MILES;
    TT(3, "footer1", "300mi");
    TT(3, "footer2", "Distance flown");
    strcpy(f.arr, "now");
    TT(0, "footer2", "Arriving now");
    strcpy(f.dep, "Charlotte Douglas Intl");             /* "Departed ..." = 31 code points */
    TT(0, "footer1", "Departed Charlotte...");
    s.mini = false;
    TT(0, "footer1", "Departed Charlotte Douglas Intl");
    s = settings();
    s.title = FW_TITLE_REGISTRATION;
    TT(0, "heading1", "N915NN");
    s = settings();
    f.sts = 'C';
    f.sexp = 1060;                                       /* 60 s left */
    TT(0, "heading3", "CANCELLED");
    f.sts = 'D';
    TT(0, "heading3", "DELAYED");
    f.sexp = 1500;                                       /* more than 120 s left: the aircraft again */
    TT(0, "heading3", "737-800");
    f.sexp = 900;                                        /* expired */
    TT(0, "heading3", "737-800");
    f.sts = 0;

    f.prog = 0;                                          /* on the ground, not yet departed */
    f.trk_gnd = true;
    fw_track_state(&f, &t);
    CHECK(t.on_ground && !t.landed);
    TT(0, "heading2", "737-800");
    TT(0, "heading3", "833kph");               /* {vel} in the default kmh */
    TT(0, "footer1", "On ground");
    TT(0, "footer2", "Preparing to takeoff");
    s.mini = false;
    CHECK(!fw_track_text(&s, &f, &t, 0, "heading3", 86, 1000, out, sizeof out));
    s.mini = true;

    f.prog = 100;                                        /* landed */
    fw_track_state(&f, &t);
    CHECK(t.landed && !t.took);
    CHECK(!fw_track_text(&s, &f, &t, 0, "footer1", 124, 1000, out, sizeof out));
    TT(0, "footer2", "Flight landed");
    strcpy(f.tft, "1h 52m");
    fw_track_state(&f, &t);
    CHECK(t.took);
    TT(2, "footer1", "Took 1h 52m");
#undef TT

    /* --- drawing ------------------------------------------------------------ */
    {
        s = settings();
        memset(fb, 0x55, sizeof fb);
        fw_screen_tracked(&S, &s, &f, 0, 1000, NULL);  /* landed: green footer2, full bar */
        CHECK(is(2, 62, 0, 255, 0) && is(125, 62, 0, 255, 0) && is(1, 62, 0, 0, 0) && is(126, 62, 0, 0, 0));
        {
            int green = 0, white = 0;
            for (int y = 52; y < 62; ++y)
                for (int x = 0; x < W; ++x) { green += is(x, y, 0, 255, 0); white += is(x, y, 255, 255, 255); }
            CHECK(green > 0 && white == 0);
        }
        f.prog = 50;
        fw_screen_tracked(&S, &s, &f, 0, 1000, NULL);
        CHECK(is(2, 62, 0, 255, 0) && is(63, 62, 0, 255, 0) && is(64, 62, 66, 65, 66) && is(125, 62, 66, 65, 66));
        f.prog = NAN;
        fw_screen_tracked(&S, &s, &f, 0, 1000, NULL);
        CHECK(is(2, 62, 66, 65, 66));
        /* text sits at y + (h - 8) / 2: heading1 box y=2 h=12 -> rows 4..11 */
        memset(fb, 0, sizeof fb);
        f = sample();
        fw_screen_flight(&S, &s, &f, NULL);
        {
            int top = H, bottom = -1;
            for (int y = 0; y < 14; ++y)
                for (int x = 40; x < 126; ++x)
                    if (is(x, y, 255, 255, 255)) { if (y < top) top = y; if (y > bottom) bottom = y; }
            CHECK(top >= 4 && bottom <= 11);
        }
    }

    /* --- several flights -------------------------------------------------- */
    CHECK(fw_max_flights_parse("off", false, false) == FW_MAXF_OFF);
    CHECK(fw_max_flights_parse("text", true, true) == FW_MAXF_TEXT);
    CHECK(fw_max_flights_parse("indicator_led", false, false) == FW_MAXF_INDICATOR);
    CHECK(fw_max_flights_parse("bogus", false, false) == FW_MAXF_TEXT);
    CHECK(fw_max_flights_parse(NULL, true, true) == FW_MAXF_OFF);
    CHECK(fw_max_flights_parse(NULL, true, false) == FW_MAXF_TEXT);
    CHECK(fw_display_speed_ms("slow") == 5000 && fw_display_speed_ms("fast") == 2000);
    CHECK(fw_display_speed_ms("normal") == 3000 && fw_display_speed_ms(NULL) == 3000);
    {
        static FwRotation r;
        static FwFlight list[5], other[3];
        const FwFlight *shown;
        int more, i;
        bool redraw;
        for (i = 0; i < 5; ++i) {
            fw_flight_init(&list[i]);
            snprintf(list[i].icao, sizeof list[i].icao, "A%d", i);
            snprintf(list[i].cs, sizeof list[i].cs, "CS%d", i);
        }
#define TICK(ms, want_show, want_icao, want_redraw) do { \
        CHECK(fw_rotation_tick(&r, (ms), &shown, &more, &redraw) == (want_show)); \
        CHECK(redraw == (want_redraw)); \
        if (want_icao) { CHECK(shown != NULL); CHECK_STR(shown->icao, (const char *)(want_icao)); } } while (0)
        fw_rotation_init(&r, FW_MAXF_OFF, 5000);
        CHECK(fw_rotation_next(&r) == NULL);
        TICK(10, FW_SHOW_NOTHING_YET, NULL, false);
        fw_rotation_set_list(&r, list, 3, 0);
        TICK(1000, FW_SHOW_FLIGHT, "A0", true);
        TICK(5999, FW_SHOW_FLIGHT, "A0", false);
        TICK(6000, FW_SHOW_FLIGHT, "A1", true);
        TICK(11000, FW_SHOW_FLIGHT, "A2", true);
        TICK(16000, FW_SHOW_FLIGHT, "A0", true);
        TICK(21000, FW_SHOW_FLIGHT, "A1", true);
        CHECK(fw_rotation_next(&r) != NULL && !strcmp(fw_rotation_next(&r)->icao, "A2"));   /* decoded ahead */
        /* a new list that still has A1: keep it, keep its time */
        other[0] = list[3]; other[1] = list[1]; other[2] = list[0];
        fw_rotation_set_list(&r, other, 3, 0);
        TICK(22000, FW_SHOW_FLIGHT, "A1", true);
        TICK(26000, FW_SHOW_FLIGHT, "A0", true);
        CHECK(!strcmp(fw_rotation_next(&r)->icao, "A3"));   /* wraps to the first */
        /* a list without the current aircraft: start at the first, full turn */
        other[0] = list[4]; other[1] = list[3];
        fw_rotation_set_list(&r, other, 2, 0);
        TICK(27000, FW_SHOW_FLIGHT, "A4", true);
        TICK(31999, FW_SHOW_FLIGHT, "A4", false);
        TICK(32000, FW_SHOW_FLIGHT, "A3", true);
        r.extra_ms = 2000;                                    /* owned: a long line still scrolling */
        TICK(37000, FW_SHOW_FLIGHT, "A3", false);
        TICK(38999, FW_SHOW_FLIGHT, "A3", false);
        r.extra_ms = 0;
        /* matched by callsign when the ICAO address is missing */
        other[0] = list[2]; other[1] = list[3]; other[1].icao[0] = 0;
        r.cur_icao[0] = 0;
        fw_rotation_set_list(&r, other, 2, 0);
        CHECK(r.index == 1 && r.since == 32000);
        /* one flight: shown once, no rotation */
        fw_rotation_set_list(&r, list, 1, 0);
        TICK(40000, FW_SHOW_FLIGHT, "A0", true);
        TICK(90000, FW_SHOW_FLIGHT, "A0", false);
        CHECK(!strcmp(fw_rotation_next(&r)->icao, "A0"));   /* one flight: itself */
        /* no flights */
        fw_rotation_set_list(&r, list, 0, 0);
        TICK(91000, FW_SHOW_NO_FLIGHTS, NULL, true);
        CHECK(fw_rotation_next(&r) == NULL);
        TICK(92000, FW_SHOW_NO_FLIGHTS, NULL, false);

        /* "text": four flights, then the too-many screen */
        fw_rotation_init(&r, FW_MAXF_TEXT, 2000);
        fw_rotation_set_list(&r, list, 5, 7);
        TICK(100, FW_SHOW_FLIGHT, "A0", true);
        TICK(2100, FW_SHOW_FLIGHT, "A1", true);
        TICK(4100, FW_SHOW_FLIGHT, "A2", true);
        TICK(6100, FW_SHOW_FLIGHT, "A3", true);
        CHECK(!strcmp(fw_rotation_next(&r)->icao, "A0"));   /* the too-many screen is skipped */
        TICK(8100, FW_SHOW_TOO_MANY, NULL, true);
        CHECK(!strcmp(fw_rotation_next(&r)->icao, "A0"));
        CHECK(more == 12);                                   /* 5 + the 7 further flights */
        fw_rotation_set_list(&r, list, 5, 9);                /* refreshed while on that screen: stay */
        TICK(8200, FW_SHOW_TOO_MANY, NULL, true);
        TICK(10100, FW_SHOW_FLIGHT, "A0", true);
        /* "indicator_led": all five, plus the corner mark */
        fw_rotation_init(&r, FW_MAXF_INDICATOR, 2000);
        fw_rotation_set_list(&r, list, 5, 3);
        CHECK(r.indicator && !r.too_many && r.cap == 5);
        fw_rotation_init(&r, FW_MAXF_OFF, 400);
        CHECK(r.dwell_ms == 500);                            /* stock floor */
#undef TICK
    }
    {
        FwFrame fr = {true, 50, true};
        memset(fb, 0x55, sizeof fb);
        fw_screen_too_many(&S, NULL, 12);
        CHECK(is(127, 0, 0, 0, 0));                          /* no border, no mark */
        {
            int orange = 0, other = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    if (is(x, y, 255, 130, 0)) ++orange;
                    else if (!is(x, y, 0, 0, 0)) ++other;
                }
            CHECK(orange > 0 && other == 0);
        }
        memset(fb, 0x55, sizeof fb);
        fw_screen_message(&S, &fr, "No flights...");        /* 13 x 6 = 78 px -> x = 25 */
        CHECK(is(0, 0, 123, 125, 123) && is(127, 63, 123, 125, 123));    /* 50 % border */
        CHECK(is(126, 0, 255, 130, 0) && is(127, 1, 255, 130, 0));       /* the mark */
        {
            int left = W, top = H;
            for (int y = 1; y < H - 1; ++y)
                for (int x = 1; x < W - 1; ++x)
                    if (is(x, y, 255, 255, 255)) { if (x < left) left = x; if (y < top) top = y; }
            CHECK(left == 25 && top == 28);
        }
        fr.indicator = false;
        fw_screen_frame(&S, &fr);
        CHECK(is(126, 0, 123, 125, 123) && is(126, 1, 0, 0, 0));
    }

    /* boot splash (SETUP.md "Boot splash"): logo box 38 rows, status strip y 46..63 */
    {
        FwSplash sp;
        const char *one[] = {"Connecting..."};
        const char *two[] = {"", "Second"};
        const FwRgb white = {255, 255, 255}, orange = {255, 166, 0};
        int left = W, right = -1, top = H, bottom = -1;
        memset(&sp, 0, sizeof sp);
        CHECK(fw_splash_logo_h(64) == 38 && fw_splash_logo_h(32) == 19);
        fill(255, 0, 0);
        fw_screen_splash(&S, &sp, one, 1, white);
        CHECK(sp.logo_drawn && is(0, 0, 0, 0, 0) && is(127, 45, 0, 0, 0));    /* first call clears the panel */
        {   /* owned: the FreeBirdWall mark in the logo box, "Wall" in the app's cyan */
            int lit = 0, cyan = 0;
            for (int y = 8; y < 8 + 38; ++y)
                for (int x = 0; x < W; ++x) {
                    lit += !is(x, y, 0, 0, 0);
                    cyan += is(x, y, 0, 200, 255);
                }
            CHECK(lit > 300 && cyan > 20);
        }
        for (int y = 46; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (is(x, y, 255, 255, 255)) {
                    if (x < left) left = x;
                    if (x > right) right = x;
                    if (y < top) top = y;
                    if (y > bottom) bottom = y;
                }
        CHECK(left >= 25 && right <= 25 + 13 * 6 - 2 && top >= 49 && bottom <= 56);   /* 13 glyphs centred at x 25 */
        CHECK(is(25, 50, 255, 255, 255));                                         /* 'C' left stroke */
        /* the same text again draws nothing; new text repaints only the strip */
        put(NULL, 5, 50, (FwRgb){0, 0, 255});
        put(NULL, 5, 10, (FwRgb){0, 0, 255});
        fw_screen_splash(&S, &sp, one, 1, white);
        CHECK(is(5, 50, 0, 0, 255));
        fw_screen_splash(&S, &sp, two, 2, white);
        CHECK(is(5, 50, 0, 0, 0) && is(5, 10, 0, 0, 255));                      /* logo area kept */
        top = H;
        for (int y = 46; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (is(x, y, 255, 255, 255) && y < top) top = y;
        CHECK(top >= 59);                                                         /* empty first line still advances */
        /* coloured text is opaque: the cell gaps are written black */
        fill(9, 9, 9);
        memset(&sp, 0, sizeof sp);
        sp.logo_drawn = true;
        fw_screen_splash(&S, &sp, one, 1, orange);
        CHECK(is(25 + 5, 49, 0, 0, 0) && is(0, 46, 0, 0, 0) && is(0, 45, 9, 9, 9));
    }

    /* "Scan to play": QR doubled at (3,3) (29 modules = 58 px), text from x 66 */
    {
        int lit_qr = 0, lit_text = 0;
        fill(9, 9, 9);
        fw_screen_play(&S, NULL, "http://192.168.1.50:8080/");
        CHECK(is(3, 3, 255, 255, 255) && is(60, 3, 255, 255, 255) && is(3, 60, 255, 255, 255));   /* finder corners */
        CHECK(is(2, 2, 0, 0, 0) && is(61, 61, 0, 0, 0));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (x < 62 && is(x, y, 255, 255, 255)) ++lit_qr;
                if (x >= 66 && !is(x, y, 0, 0, 0)) ++lit_text;
            }
        CHECK(lit_qr > 800 && lit_text > 100);
    }

    /* dashboard sign-in code: digits doubled and centred, countdown below */
    {
        int left = W, right = -1, top = H;
        fill(9, 9, 9);
        fw_screen_pair_code(&S, "042917", 105);
        for (int y = 20; y < 40; ++y)
            for (int x = 0; x < W; ++x)
                if (is(x, y, 255, 255, 255)) { if (x < left) left = x; if (x > right) right = x; if (y < top) top = y; }
        CHECK(left >= 28 && right <= 99 && top >= 22);                /* 6 x 12 = 72 px from x 28 */
        CHECK(!is(0, 0, 9, 9, 9));                                   /* cleared */
        {
            int cyan = 0, grey = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) { cyan += is(x, y, 0, 255, 255); grey += is(x, y, 150, 150, 150); }
            CHECK(cyan > 20 && grey > 20);
        }
    }

    /* owned airport aliases (not a vendor feature) */
    {
        char out[FW_FLIGHT_TEXT_MAX];
        FwFlightSettings st = settings();
        FwFlight g = sample();
        strcpy(g.dest_iata, "DJT");
        strcpy(g.dest, "Donald J Trump Intl");
        fw_flight_expand(&st, &g, "{orig_iata}-{dest_iata} {dest}", out, sizeof out);
        CHECK_STR(out, "CLT-DJT Donald J Trump Intl");              /* no hook: as the feed says */
        st.alias = test_alias;
        alias_calls = 0;
        fw_flight_expand(&st, &g, "{orig_iata}-{dest_iata} {dest}", out, sizeof out);
        CHECK_STR(out, "CLT-PBI Palm Beach Intl");
        CHECK(alias_calls == 4);                                   /* orig, dest, orig_iata, dest_iata */
        fw_flight_expand(&st, &g, "{reg} {al_name}", out, sizeof out);
        CHECK_STR(out, "N915NN American");                         /* other tokens untouched */
        strcpy(g.orig_iata, "DJT");
        fw_flight_expand(&st, &g, "{orig_iata}-{dest_iata}", out, sizeof out);
        CHECK_STR(out, "PBI-PBI");
    }

    printf("PASS: %d flight checks\n", checks);
    return 0;
}
