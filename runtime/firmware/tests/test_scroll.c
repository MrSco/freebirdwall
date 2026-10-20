/* Long text (FLIGHT-SCREENS.md "Long text"): shortened names, then lines that
 * still do not fit scroll inside their box (components/fw_screens). */
#include <stdio.h>
#include <string.h>
#include "fw_screens.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned char px[64][128];       /* 1 = lit */
static int writes_outside, writes;
static int box_x0, box_x1, box_y0, box_y1;

static void pixel(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    px[y][x] = c.r || c.g || c.b;
    ++writes;
    if (x < box_x0 || x >= box_x1 || y < box_y0 || y >= box_y1) ++writes_outside;
}

static FwFlightSettings settings(void){
    FwFlightSettings s;
    memset(&s, 0, sizeof s);
    s.mini = true;
    s.title = FW_TITLE_AIRLINE;
    s.layout = "DEFAULT";
    return s;
}

static int lit_columns(int x0, int x1, int y0, int y1){
    int n = 0;
    for (int x = x0; x < x1; ++x)
        for (int y = y0; y < y1; ++y) if (px[y][x]) { ++n; break; }
    return n;
}

int main(void){
    FwSurface s = {NULL, 128, 64, pixel, NULL};
    FwFlightSettings st = settings();
    FwFlight f;

    /* timing: still, then a pixel per FW_SCROLL_PX_MS, still at the end, again */
    CHECK(fw_scroll_offset(100, 124, 5000) == 0);                      /* fits: never moves */
    CHECK(fw_scroll_offset(150, 124, 0) == 0);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS - 1) == 0);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 10 * FW_SCROLL_PX_MS) == 10);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 26 * FW_SCROLL_PX_MS) == 26);   /* the end shows */
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 26 * FW_SCROLL_PX_MS + FW_SCROLL_END_MS - 1) == 26);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 26 * FW_SCROLL_PX_MS + FW_SCROLL_END_MS) == 0);
    /* speeds: the owner's slow / normal / fast are 80 / 40 / 20 ms a pixel */
    fw_screen_scroll_speed(20);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 10 * 20) == 10);
    fw_screen_scroll_speed(80);
    CHECK(fw_scroll_offset(150, 124, FW_SCROLL_HOLD_MS + 10 * 80) == 10);
    fw_screen_scroll_speed(FW_SCROLL_PX_MS);

    /* a card with a long origin: it is shortened first, then what is left scrolls */
    fw_flight_init(&f);
    snprintf(f.al_name, sizeof f.al_name, "JetBlue Airways");
    snprintf(f.cs, sizeof f.cs, "JBU123");
    snprintf(f.orig_iata, sizeof f.orig_iata, "FLL");
    snprintf(f.dest_iata, sizeof f.dest_iata, "RDU");
    snprintf(f.orig, sizeof f.orig, "Fort Lauderdale Hollywood Intl");
    snprintf(f.dest, sizeof f.dest, "Raleigh Durham International");
    snprintf(f.ac_name, sizeof f.ac_name, "Airbus A-320");
    memset(px, 0, sizeof px);
    box_x0 = 0; box_x1 = 128; box_y0 = 0; box_y1 = 64;
    fw_screen_flight(&s, &st, &f, NULL);
    CHECK(fw_screen_scroll_count() == 1);                              /* only "Fort Lauderdale Hollywood" */
    /* 150 px in a 124 px box: the card is kept until it has scrolled through once */
    CHECK(fw_screen_scroll_pass_ms() == FW_SCROLL_HOLD_MS + 26 * FW_SCROLL_PX_MS + FW_SCROLL_END_MS);
    CHECK(lit_columns(126, 128, 40, 50) == 0);                         /* clipped to the footer box */
    CHECK(lit_columns(40, 126, 2, 14) > 30);                           /* "JetBlue" fits now */

    /* only the scrolling line is redrawn, only inside its box */
    box_x0 = 2; box_x1 = 126; box_y0 = 40; box_y1 = 50;
    writes = writes_outside = 0;
    CHECK(!fw_screen_scroll_tick(&s, 0));                              /* still at first */
    CHECK(writes == 0);
    CHECK(fw_screen_scroll_tick(&s, FW_SCROLL_HOLD_MS + 5 * FW_SCROLL_PX_MS));
    CHECK(writes > 0 && writes_outside == 0);
    CHECK(!fw_screen_scroll_tick(&s, FW_SCROLL_HOLD_MS + 5 * FW_SCROLL_PX_MS + 1));   /* same offset: nothing */
    /* at the end the line's last letters reach the box's right edge, nothing beyond */
    CHECK(fw_screen_scroll_tick(&s, FW_SCROLL_HOLD_MS + 26 * FW_SCROLL_PX_MS));
    CHECK(lit_columns(118, 126, 40, 50) > 0 && writes_outside == 0);

    /* any other full screen forgets the lines */
    {
        static const FwFrame frame = {false, 50, false};
        fw_screen_message(&s, &frame, "No flights...");
    }
    CHECK(fw_screen_scroll_count() == 0 && fw_screen_scroll_pass_ms() == 0);
    CHECK(!fw_screen_scroll_tick(&s, FW_SCROLL_HOLD_MS + 5 * FW_SCROLL_PX_MS));

    /* the tracked card scrolls its headings too */
    snprintf(f.ac_name, sizeof f.ac_name, "Bombardier CRJ-900LR");
    f.has_trk = true;
    f.prog = 40;
    fw_screen_tracked(&s, &st, &f, 0, 0, NULL);
    CHECK(fw_screen_scroll_count() == 1);
    fw_screen_scroll_reset();
    CHECK(fw_screen_scroll_count() == 0);

    printf("%s: %d scroll checks\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
