/* Desktop tests for the games session (fw_arcade) and the offscreen canvas.
 * Run: py tests/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_arcade.h"
#include "fw_canvas.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_, b_); exit(1); } ++checks; } while (0)

static FwArcade A;

static bool msg(int slot, const char *m, uint32_t now){ return fw_arcade_message(&A, slot, m, strlen(m), now); }

static void test_slots_and_messages(void){
    fw_arcade_init(&A, 128, 64, 5);
    CHECK(!fw_arcade_active(&A));
    CHECK(fw_arcade_join(&A, 0) == 0 && fw_arcade_join(&A, 0) == 1 && fw_arcade_join(&A, 0) == -1);
    fw_arcade_leave(&A, 0, 10);
    CHECK(fw_arcade_join(&A, 20) == 0);                       /* the freed slot */
    fw_arcade_leave(&A, 5, 0);                                /* out of range: ignored */
    fw_arcade_leave(&A, 1, 30);
    CHECK(!msg(1, "A", 40));                                  /* not joined */

    CHECK(msg(0, "T 32", 50) && A.player[0].target == 32);
    CHECK(msg(0, "T -1", 50) && A.player[0].target == FW_GAME_NO_TARGET);
    CHECK(msg(0, "T 0", 50) && A.player[0].target == 0);
    CHECK(msg(0, "D -1", 50) && A.player[0].dir == -1);
    CHECK(msg(0, "D 1", 50) && A.player[0].dir == 1);
    CHECK(msg(0, "A", 50) && A.player[0].action);
    {
        static const char *const bad[] = {"", "T", "T ", "T 3a", "T 1001", "T -2", "T12", "D 2", "D -", "A ", "AA",
                                          "G", "G x", "G pp", "Gp", "X ", "Q", "T 99999999999999999999"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(!msg(0, bad[i], 60));
        CHECK(!fw_arcade_message(&A, 0, NULL, 3, 60));
        CHECK(!fw_arcade_message(&A, 0, "T 3", 0, 60));
    }
    CHECK(A.last_input == 50);                                /* invalid messages are not activity */
}

static void test_pong_session(void){
    char st[160];
    uint32_t serial;
    fw_arcade_init(&A, 128, 64, 5);
    CHECK(fw_arcade_join(&A, 1000) == 0);
    fw_arcade_status(&A, 0, st, sizeof st);
    CHECK_STR(st, "{\"game\":\"none\",\"you\":0,\"players\":1}");
    CHECK(msg(0, "G p", 1000) && fw_arcade_active(&A) && A.game == FW_ARCADE_PONG);
    CHECK(fw_arcade_advance(&A, 1000) && A.pong.ticks == 1);  /* due at once */
    CHECK(fw_arcade_advance(&A, 1019) && A.pong.ticks == 1);
    CHECK(fw_arcade_advance(&A, 1020) && A.pong.ticks == 2);
    CHECK(fw_arcade_advance(&A, 1100) && A.pong.ticks == 6);  /* 20 ms each */
    CHECK(A.pong.cpu[1] && !A.pong.cpu[0]);                   /* one phone: the computer plays the right */
    fw_arcade_status(&A, 0, st, sizeof st);
    CHECK_STR(st, "{\"game\":\"pong\",\"phase\":\"ready\",\"you\":0,\"players\":1,\"score\":[0,0]}");

    /* action is one tick only: it serves */
    CHECK(msg(0, "A", 1110));
    fw_arcade_advance(&A, 1120);
    CHECK(A.pong.phase == FW_GAME_PLAYING && !A.player[0].action);
    fw_arcade_status(&A, 0, st, sizeof st);
    CHECK_STR(st, "{\"game\":\"pong\",\"phase\":\"playing\",\"you\":0,\"players\":1,\"score\":[0,0]}");

    /* the drag moves the left paddle towards the finger */
    CHECK(msg(0, "T 10", 1130));
    fw_arcade_advance(&A, 1300);
    CHECK(A.pong.paddle_y[0] == 4 * 256);

    /* a stall runs at most 10 ticks, then skips ahead */
    {
        uint32_t before = A.pong.ticks;
        fw_arcade_advance(&A, 5000);
        CHECK(A.pong.ticks == before + FW_ARCADE_MAX_CATCHUP);
        CHECK(A.next_tick == 5000 + FW_GAME_TICK_MS);
    }
    /* a second phone takes the right paddle from the computer */
    CHECK(fw_arcade_join(&A, 5000) == 1);
    fw_arcade_advance(&A, 5020);
    CHECK(!A.pong.cpu[1]);
    /* points change the serial (status worth sending) */
    serial = A.serial;
    A.pong.phase = FW_GAME_PLAYING;
    A.pong.ball_x = 2 * 256; A.pong.ball_vx = -2 * 256; A.pong.ball_vy = 0; A.pong.paddle_y[0] = 0; A.pong.ball_y = 50 * 256;
    for (uint32_t t = 5040; t <= 5100; t += 20) fw_arcade_advance(&A, t);
    CHECK(A.pong.score[1] == 1 && A.serial != serial);
    fw_arcade_status(&A, 1, st, sizeof st);
    CHECK_STR(st, "{\"game\":\"pong\",\"phase\":\"ready\",\"you\":1,\"players\":2,\"score\":[0,1]}");

    /* "X" ends it */
    CHECK(msg(1, "X", 5200) && !fw_arcade_active(&A) && !fw_arcade_advance(&A, 5220));
}

static void test_breakout_session(void){
    char st[160];
    fw_arcade_init(&A, 128, 64, 9);
    fw_arcade_join(&A, 0);
    CHECK(fw_arcade_join(&A, 0) == 1);
    fw_arcade_leave(&A, 0, 0);                                /* only the second phone is left */
    CHECK(msg(1, "G b", 0) && A.game == FW_ARCADE_BREAKOUT);
    CHECK(msg(1, "T 100", 0));
    for (uint32_t t = 0; t <= 400; t += 20) fw_arcade_advance(&A, t);
    CHECK(A.breakout.paddle_x == 92 * 256);                   /* centre 100 - 8: it steers the paddle */
    fw_arcade_status(&A, 1, st, sizeof st);
    CHECK_STR(st, "{\"game\":\"breakout\",\"phase\":\"ready\",\"you\":1,\"players\":1,\"score\":0,\"lives\":3,\"level\":1}");
    CHECK(msg(1, "A", 420));
    fw_arcade_advance(&A, 420);
    CHECK(A.breakout.phase == FW_GAME_PLAYING);
}

static void test_timeouts(void){
    fw_arcade_init(&A, 128, 64, 3);
    fw_arcade_join(&A, 0);
    msg(0, "G p", 0);
    CHECK(fw_arcade_advance(&A, FW_ARCADE_IDLE_MS - 20));
    CHECK(!fw_arcade_advance(&A, FW_ARCADE_IDLE_MS) && !fw_arcade_active(&A));   /* nobody played for a minute */

    fw_arcade_init(&A, 128, 64, 3);
    fw_arcade_join(&A, 0);
    msg(0, "G b", 0);
    fw_arcade_leave(&A, 0, 1000);
    CHECK(fw_arcade_advance(&A, 1000 + FW_ARCADE_EMPTY_MS - 20));
    CHECK(fw_arcade_join(&A, 1000 + FW_ARCADE_EMPTY_MS - 10) == 0);          /* back in time */
    CHECK(fw_arcade_advance(&A, 1000 + FW_ARCADE_EMPTY_MS + 5000));
    fw_arcade_leave(&A, 0, 30000);
    CHECK(!fw_arcade_advance(&A, 30000 + FW_ARCADE_EMPTY_MS));                /* everyone left */
}

/* ------------------------------------------------------------ canvas */

static int pixel_calls, fill_calls, fill_pixels;
static FwRgb panel[64][128];
static void ppix(void *ctx, int x, int y, FwRgb c){ (void)ctx; panel[y][x] = c; ++pixel_calls; }
static void pfill(void *ctx, int x, int y, int w, int h, FwRgb c){
    (void)ctx;
    for (int j = y; j < y + h; ++j) for (int i = x; i < x + w; ++i) panel[j][i] = c;
    ++fill_calls; fill_pixels += w * h;
}

static void test_canvas(void){
    static uint16_t frame[128 * 64], shown[128 * 64];
    FwCanvas c;
    FwSurface s, p = {NULL, 128, 64, ppix, pfill}, p_nofill = {NULL, 128, 64, ppix, NULL};
    const FwRgb white = {255, 255, 255}, amber = {255, 166, 0}, red = {255, 0, 0};
    CHECK(fw_rgb565(white) == 0xFFFF && fw_rgb565(amber) == 0xFD20 && fw_rgb565(red) == 0xF800);
    CHECK(fw_rgb888(0xFD20).r == 255 && fw_rgb888(0xFD20).g == 166 && fw_rgb888(0xFD20).b == 0);
    fw_canvas_init(&c, 128, 64, frame, shown);
    s = fw_canvas_surface(&c);
    /* first flush writes everything, in one run per row */
    CHECK(fw_canvas_flush(&c, &p) == 128 * 64 && fill_calls == 64 && pixel_calls == 0);
    fill_calls = 0;
    CHECK(fw_canvas_flush(&c, &p) == 0 && fill_calls == 0);  /* nothing changed */
    fw_fill_rect(&s, 10, 5, 3, 2, white);                     /* through the surface */
    s.pixel(s.ctx, 40, 6, red);
    CHECK(fw_canvas_flush(&c, &p) == 7);
    CHECK(fill_calls == 2 && fill_pixels == 128 * 64 + 6 && pixel_calls == 1);
    CHECK(panel[5][10].r == 255 && panel[6][12].b == 255 && panel[6][40].g == 0 && panel[6][40].r == 255);
    /* redrawing the same picture from scratch costs nothing */
    fw_clear(&s);
    fw_fill_rect(&s, 10, 5, 3, 2, white);
    s.pixel(s.ctx, 40, 6, red);
    CHECK(fw_canvas_flush(&c, &p) == 0);
    /* a moved box: only the old and new pixels */
    fw_clear(&s);
    fw_fill_rect(&s, 11, 5, 3, 2, white);
    CHECK(fw_canvas_flush(&c, &p_nofill) == 2 + 2 + 1);       /* column 10 off, 13 on, red dot off */
    CHECK(panel[5][10].r == 0 && panel[5][13].r == 255);
    /* someone else drew the panel: everything again */
    fw_canvas_invalidate(&c);
    CHECK(fw_canvas_flush(&c, &p) == 128 * 64);
    /* the canvas draws exactly what the panel would (565 precision) */
    {
        FwPong g;
        fw_pong_init(&g, 128, 64, 1);
        fw_pong_draw(&g, &s);
        fw_canvas_flush(&c, &p);
        CHECK(panel[31][63].r == 255 && panel[0][3].r == 0 && panel[26][3].g == 255);
    }
}

int main(void){
    test_slots_and_messages();
    test_pong_session();
    test_breakout_session();
    test_timeouts();
    test_canvas();
    printf("PASS: %d arcade checks\n", checks);
    return 0;
}
