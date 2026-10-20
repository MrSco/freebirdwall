/* Desktop tests for components/fw_games (Pong and Breakout).
 * Run: py tests/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_games.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)

#define W 128
#define H 64
#define Q 256
static unsigned char fb[H][W][3];
static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
}
static const FwSurface S = {NULL, W, H, put, NULL};
static int is(int x, int y, int r, int g, int b){ return fb[y][x][0] == r && fb[y][x][1] == g && fb[y][x][2] == b; }
static int lit(int x, int y){ return fb[y][x][0] || fb[y][x][1] || fb[y][x][2]; }
static int lit_in(int x0, int y0, int w, int h){
    int n = 0;
    for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) n += lit(x, y);
    return n;
}

static FwGameInput idle(void){
    FwGameInput in;
    memset(&in, 0, sizeof in);
    in.p[0].target = in.p[1].target = FW_GAME_NO_TARGET;
    return in;
}

static uint32_t rnd(uint32_t *s){ *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static FwGameInput random_input(uint32_t *s, int players){
    FwGameInput in = idle();
    for (int i = 0; i < players; ++i) {
        uint32_t r = rnd(s);
        in.p[i].present = true;
        in.p[i].dir = (int8_t)((int)(r % 3) - 1);
        in.p[i].target = (int16_t)((r >> 4) % 4 == 0 ? (int)((r >> 8) % 140) - 6 : FW_GAME_NO_TARGET);
        in.p[i].action = (r >> 16) % 50 == 0;
    }
    return in;
}

/* ------------------------------------------------------------ Pong */

static void test_pong_basics(void){
    FwPong g;
    FwGameInput in = idle();
    fw_pong_init(&g, W, H, 42);
    CHECK(g.phase == FW_GAME_READY);
    CHECK(g.ball_x == 63 * Q && g.ball_y == 31 * Q && g.ball_vx == 0);
    CHECK(g.paddle_y[0] == 26 * Q && g.paddle_y[1] == 26 * Q);
    CHECK(fw_pong_paddle_x(&g, 0) == 3 && fw_pong_paddle_x(&g, 1) == 123);

    /* automatic serve after 2 s, or at once on action */
    for (int i = 0; i < 99; ++i) fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_READY);
    fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_PLAYING);
    CHECK(g.ball_vx == (g.serve_to ? Q : -Q));
    CHECK(g.ball_vy >= -3 * Q / 4 && g.ball_vy <= 3 * Q / 4);
    CHECK(g.cpu[1] && !g.cpu[0] == false);   /* no phone on either side: both computer */

    fw_pong_init(&g, W, H, 42);
    in.p[0].action = true;
    fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_PLAYING);
}

static void test_pong_paddles(void){
    FwPong g;
    FwGameInput in = idle();
    fw_pong_init(&g, W, H, 7);
    in.p[0].present = in.p[1].present = true;
    in.p[0].dir = 1;
    fw_pong_tick(&g, &in);
    CHECK(g.paddle_y[0] == 26 * Q + 3 * Q / 2);
    CHECK(g.paddle_y[1] == 26 * Q);
    for (int i = 0; i < 100; ++i) fw_pong_tick(&g, &in);
    CHECK(g.paddle_y[0] == (H - FW_PONG_PADDLE_H) * Q);          /* stops at the bottom */
    in.p[0].dir = 0;
    in.p[0].target = 10;                                          /* drag: centre to y=10 */
    for (int i = 0; i < 16; ++i) fw_pong_tick(&g, &in);       /* 48 px at 3 px per tick */
    CHECK(g.paddle_y[0] == 4 * Q);
    in.p[0].target = -50;
    for (int i = 0; i < 30; ++i) fw_pong_tick(&g, &in);
    CHECK(g.paddle_y[0] == 0);                                    /* clamped at the top */
    /* the drag moves at most 3 px per tick */
    in.p[0].target = 63;
    fw_pong_tick(&g, &in);
    CHECK(g.paddle_y[0] == 3 * Q);
}

static void test_pong_hits(void){
    FwPong g;
    FwGameInput in = idle();
    in.p[0].present = in.p[1].present = true;

    /* centre hit on the left paddle: back across, faster, flat */
    fw_pong_init(&g, W, H, 3);
    g.phase = FW_GAME_PLAYING;
    g.paddle_y[0] = 26 * Q;
    g.ball_x = 5 * Q + Q / 2; g.ball_y = 31 * Q; g.ball_vx = -Q; g.ball_vy = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_vx == Q + Q / 16);
    CHECK(g.ball_vy == 0);
    CHECK(g.ball_x == 5 * Q);

    /* top-edge hit: leaves upwards, steeply */
    fw_pong_init(&g, W, H, 3);
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 5 * Q + Q / 2; g.ball_y = 25 * Q; g.ball_vx = -Q; g.ball_vy = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_vx > 0 && g.ball_vy < 0);
    CHECK(g.ball_vy == -(Q + Q / 16) * 3 / 4);                   /* edge: 3/4 of the speed */

    /* right paddle */
    fw_pong_init(&g, W, H, 3);
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 120 * Q + Q / 2; g.ball_y = 31 * Q; g.ball_vx = Q; g.ball_vy = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_vx == -(Q + Q / 16));
    CHECK(g.ball_x == 121 * Q);

    /* speed is capped */
    fw_pong_init(&g, W, H, 3);
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 7 * Q; g.ball_y = 31 * Q; g.ball_vx = -3 * Q; g.ball_vy = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_vx == 3 * Q);

    /* walls */
    fw_pong_init(&g, W, H, 3);
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 60 * Q; g.ball_y = Q / 2; g.ball_vx = Q; g.ball_vy = -Q;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_y == Q / 2 && g.ball_vy == Q);
    g.ball_y = 61 * Q + Q / 2; g.ball_vy = Q;
    fw_pong_tick(&g, &in);
    CHECK(g.ball_y == 61 * Q + Q / 2 && g.ball_vy == -Q);
}

static void test_pong_points(void){
    FwPong g;
    FwGameInput in = idle();
    in.p[0].present = in.p[1].present = true;
    fw_pong_init(&g, W, H, 9);
    g.phase = FW_GAME_PLAYING;
    g.paddle_y[0] = 0;
    g.ball_x = 2 * Q; g.ball_y = 50 * Q; g.ball_vx = -2 * Q; g.ball_vy = 0;
    for (int i = 0; i < 3; ++i) fw_pong_tick(&g, &in);
    CHECK(g.score[1] == 1 && g.score[0] == 0);
    CHECK(g.phase == FW_GAME_READY && g.serve_to == 0 && g.wait == 50);
    CHECK(g.ball_x == 63 * Q && g.ball_vx == 0);

    /* first to 11 */
    g.score[0] = 10;
    g.phase = FW_GAME_PLAYING;
    g.paddle_y[1] = 0;
    g.ball_x = 125 * Q; g.ball_y = 50 * Q; g.ball_vx = 2 * Q; g.ball_vy = 0;
    for (int i = 0; i < 3; ++i) fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_OVER && g.winner == 0 && g.score[0] == 11);
    for (int i = 0; i < 200; ++i) fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_OVER);                               /* waits for a player */
    in.p[1].action = true;
    fw_pong_tick(&g, &in);
    CHECK(g.phase == FW_GAME_READY && g.score[0] == 0 && g.score[1] == 0);
}

static void test_pong_cpu(void){
    FwPong g;
    FwGameInput in = idle();
    in.p[0].present = true;
    fw_pong_init(&g, W, H, 11);
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 100 * Q; g.ball_y = 5 * Q; g.ball_vx = Q / 4; g.ball_vy = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.cpu[1] && !g.cpu[0]);
    CHECK(g.paddle_y[1] == 26 * Q - 5 * Q / 4);                   /* chases at 1.25 px per tick */
    /* ball going away: back towards the middle */
    g.ball_vx = -Q / 4;
    g.paddle_y[1] = 0;
    fw_pong_tick(&g, &in);
    CHECK(g.paddle_y[1] == 5 * Q / 4);
}

/* Long random play: the ball stays in the court, scores stay bounded, games end. */
static void test_pong_soak(void){
    uint32_t s = 1;
    int overs = 0;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        FwPong g;
        fw_pong_init(&g, W, H, seed);
        for (int t = 0; t < 120000; ++t) {
            FwGameInput in = random_input(&s, (int)(seed % 3));
            FwGamePhase before = g.phase;
            fw_pong_tick(&g, &in);
            if (g.phase == FW_GAME_OVER && before != FW_GAME_OVER) ++overs;
            if (g.ball_y < 0 || g.ball_y > (H - 2) * Q || g.paddle_y[0] < 0 || g.paddle_y[1] < 0 ||
                g.paddle_y[0] > (H - 12) * Q || g.paddle_y[1] > (H - 12) * Q || g.score[0] > 11 || g.score[1] > 11 ||
                g.ball_vx > 3 * Q || g.ball_vx < -3 * Q) {
                fprintf(stderr, "pong invariant broken seed %u tick %d\n", seed, t);
                CHECK(0);
            }
            /* a serve always crosses the court towards a side */
            if (g.phase == FW_GAME_PLAYING && g.ball_vx == 0) CHECK(0);
        }
    }
    CHECK(overs > 10);
    ++checks;
}

static void test_pong_replay(void){
    FwPong a, b;
    uint32_t s1 = 5, s2 = 5;
    fw_pong_init(&a, W, H, 77);
    fw_pong_init(&b, W, H, 77);
    for (int t = 0; t < 20000; ++t) {
        FwGameInput i1 = random_input(&s1, 1), i2 = random_input(&s2, 1);
        fw_pong_tick(&a, &i1);
        fw_pong_tick(&b, &i2);
    }
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}

static void test_pong_draw(void){
    FwPong g;
    fw_pong_init(&g, W, H, 1);
    g.score[0] = 3; g.score[1] = 10;
    g.paddle_y[0] = 0;
    fw_pong_draw(&g, &S);
    CHECK(is(3, 0, 255, 255, 255) && is(4, 11, 255, 255, 255) && !lit(3, 12) && !lit(5, 0));   /* left paddle */
    CHECK(is(123, 26, 255, 255, 255) && is(124, 37, 255, 255, 255) && !lit(123, 25));        /* right paddle */
    CHECK(is(63, 31, 255, 255, 255) && is(64, 32, 255, 255, 255));                           /* ball */
    CHECK(is(64, 40, 70, 70, 70) && is(64, 41, 70, 70, 70) && !lit(64, 42));                /* net */
    CHECK(lit_in(29, 1, 6, 7) > 0 && lit_in(90, 1, 12, 7) > 0);                             /* scores */
    CHECK(lit_in(96 - 6, 1, 12, 7) == lit_in(65, 0, W - 65, 8));            /* "10" centred at 96 */

    g.phase = FW_GAME_OVER; g.winner = 1; g.cpu[1] = true;
    fw_pong_draw(&g, &S);
    CHECK(lit_in(40, 28, 48, 8) > 20);
    {   /* "CPU WINS": 8 glyphs centred, yellow on black */
        int yellow = 0;
        for (int y = 28; y < 36; ++y) for (int x = 40; x < 88; ++x) yellow += is(x, y, 255, 200, 0);
        CHECK(yellow > 20);
        CHECK(!lit(63, 20));                                     /* no ball once the game is over */
    }
}

/* ------------------------------------------------------------ Breakout */

static int popcount8(uint8_t v){ int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static void test_breakout_basics(void){
    FwBreakout g;
    FwGameInput in = idle();
    fw_breakout_init(&g, W, H, 5);
    CHECK(g.bricks_left == 48 && g.lives == 3 && g.level == 1 && g.score == 0);
    CHECK(g.phase == FW_GAME_READY && g.paddle_w == 16 && g.paddle_x == 56 * Q);
    CHECK(fw_breakout_brick_w(&g) == 15 && fw_breakout_brick_x(&g, 7) == 112 && fw_breakout_brick_y(5) == 30);
    fw_breakout_tick(&g, &in);
    CHECK(g.ball_x == 63 * Q && g.ball_y == 59 * Q);              /* resting on the paddle */
    in.p[0].action = true;
    fw_breakout_tick(&g, &in);
    CHECK(g.phase == FW_GAME_PLAYING && g.ball_vy < 0);
    CHECK(g.ball_vx >= -230 * 3 / 8 && g.ball_vx <= 230 * 3 / 8);

    /* automatic launch after 1.5 s */
    fw_breakout_init(&g, W, H, 5);
    in.p[0].action = false;
    for (int i = 0; i < 74; ++i) fw_breakout_tick(&g, &in);
    CHECK(g.phase == FW_GAME_READY);
    fw_breakout_tick(&g, &in);
    CHECK(g.phase == FW_GAME_PLAYING);
}

static void test_breakout_bricks(void){
    FwBreakout g;
    FwGameInput in = idle();
    fw_breakout_init(&g, W, H, 5);
    g.phase = FW_GAME_PLAYING;
    /* straight up into the bottom row, column 3 (x 48..62, y 30..32) */
    g.ball_x = 50 * Q; g.ball_y = 33 * Q + Q / 2; g.ball_vx = 0; g.ball_vy = -Q; g.speed = Q;
    fw_breakout_tick(&g, &in);
    CHECK(!(g.bricks[5] & (1u << 3)) && g.bricks_left == 47 && g.score == 1);
    /* two half-pixel sub-steps: the second touched the brick and is undone */
    CHECK(g.ball_vy == Q && g.ball_y == 33 * Q);
    fw_breakout_tick(&g, &in);
    CHECK(g.ball_y == 34 * Q && g.bricks_left == 47);

    /* sideways into a brick: reflects across */
    fw_breakout_init(&g, W, H, 5);
    g.phase = FW_GAME_PLAYING;
    g.bricks[5] = 0;                                              /* open the bottom row */
    g.ball_x = 13 * Q; g.ball_y = 31 * Q; g.ball_vx = Q; g.ball_vy = 0; g.speed = Q;
    g.bricks[5] = (uint8_t)(1u << 1);                             /* one brick at x 16..30 */
    g.bricks_left = 41;
    fw_breakout_tick(&g, &in);
    fw_breakout_tick(&g, &in);
    CHECK(g.bricks[5] == 0 && g.ball_vx == -Q && g.ball_x == 14 * Q + Q / 2);

    /* top two rows: one speed-up, worth 7 */
    fw_breakout_init(&g, W, H, 5);
    g.phase = FW_GAME_PLAYING;
    for (int r = 2; r < 6; ++r) g.bricks[r] = 0;
    g.bricks_left = 16;
    g.ball_x = 50 * Q; g.ball_y = 18 * Q; g.ball_vx = 0; g.ball_vy = -Q; g.speed = Q;
    for (int i = 0; i < 4; ++i) fw_breakout_tick(&g, &in);
    CHECK(g.score == 7 && g.hit_top_rows && g.speed == Q + 40 && g.ball_vy == Q + 40);
}

static void test_breakout_paddle(void){
    FwBreakout g;
    FwGameInput in = idle();
    fw_breakout_init(&g, W, H, 5);
    g.phase = FW_GAME_PLAYING;
    for (int r = 0; r < 6; ++r) g.bricks[r] = 0;
    g.bricks[0] = 1;
    g.bricks_left = 1;
    g.speed = 230;
    g.ball_x = 63 * Q; g.ball_y = 59 * Q; g.ball_vx = 0; g.ball_vy = 230;
    fw_breakout_tick(&g, &in);
    CHECK(g.hits == 1 && g.ball_vy == -230 && g.ball_vx == 0);   /* centre: straight up */
    /* right end: off to the right */
    g.ball_x = 70 * Q; g.ball_y = 59 * Q; g.ball_vx = 0; g.ball_vy = 230;
    fw_breakout_tick(&g, &in);
    CHECK(g.ball_vx > 0 && g.ball_vy < 0 && g.ball_vy <= -230 / 2);
    /* 4th hit speeds up */
    g.hits = 3;
    g.ball_x = 63 * Q; g.ball_y = 59 * Q; g.ball_vx = 0; g.ball_vy = 230;
    fw_breakout_tick(&g, &in);
    CHECK(g.hits == 4 && g.speed == 270 && g.ball_vy == -270);
    /* the ceiling shrinks the paddle once */
    g.ball_x = 100 * Q; g.ball_y = 8 * Q + 10; g.ball_vx = 0; g.ball_vy = -270;
    fw_breakout_tick(&g, &in);
    CHECK(g.hit_ceiling && g.paddle_w == 10 && g.ball_vy == 270);

    /* drag the paddle */
    in.p[0].present = true;
    in.p[0].target = 100;                                         /* centre at 100: left edge 95 */
    for (int i = 0; i < 40; ++i) fw_breakout_tick(&g, &in);
    CHECK(g.paddle_x == 95 * Q);
    in.p[0].target = 126;                                         /* past the edge: clamped */
    for (int i = 0; i < 10; ++i) fw_breakout_tick(&g, &in);
    CHECK(g.paddle_x == (W - 10) * Q);
}

static void test_breakout_lives_and_levels(void){
    FwBreakout g;
    FwGameInput in = idle();
    fw_breakout_init(&g, W, H, 5);
    for (int life = 3; life >= 1; --life) {
        g.phase = FW_GAME_PLAYING;
        g.paddle_x = 0;
        g.ball_x = 100 * Q; g.ball_y = 62 * Q; g.ball_vx = 0; g.ball_vy = Q;
        for (int i = 0; i < 3; ++i) fw_breakout_tick(&g, &in);
        CHECK(g.lives == life - 1);
        CHECK(g.phase == (life > 1 ? FW_GAME_READY : FW_GAME_OVER));
    }
    in.p[0].action = true;
    fw_breakout_tick(&g, &in);
    CHECK(g.phase == FW_GAME_READY && g.lives == 3 && g.score == 0 && g.bricks_left == 48);

    /* clearing the last brick builds the next, faster wall */
    fw_breakout_init(&g, W, H, 5);
    for (int r = 0; r < 6; ++r) g.bricks[r] = 0;
    g.bricks[5] = 1u << 3;
    g.bricks_left = 1;
    g.phase = FW_GAME_PLAYING;
    g.ball_x = 50 * Q; g.ball_y = 33 * Q + Q / 2; g.ball_vx = 0; g.ball_vy = -Q; g.speed = Q;
    in.p[0].action = false;
    fw_breakout_tick(&g, &in);
    CHECK(g.level == 2 && g.bricks_left == 48 && g.phase == FW_GAME_READY && g.score == 1);
    CHECK(g.base_speed == 250 && g.speed == 250 && g.paddle_w == 16);
}

/* A good player (follows the ball, hits off-centre) clears walls, and the
 * ball never sits inside a brick or leaves the side walls. */
static void test_breakout_soak(void){
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        FwBreakout g;
        uint32_t s = seed;
        fw_breakout_init(&g, W, H, seed);
        for (int t = 0; t < 200000 && g.level < 3; ++t) {
            FwGameInput in = idle();
            int bricks = 0;
            in.p[0].present = true;
            in.p[0].target = (int16_t)(g.ball_x / Q + 1 + (int)(rnd(&s) % 13) - 6);
            fw_breakout_tick(&g, &in);
            for (int r = 0; r < 6; ++r) bricks += popcount8(g.bricks[r]);
            CHECK(bricks == g.bricks_left);
            if (g.ball_x < 0 || g.ball_x > (W - 2) * Q || g.ball_y < 8 * Q) CHECK(0);
            if (g.phase == FW_GAME_PLAYING) {
                int x0 = (int)(g.ball_x / Q), y0 = (int)(g.ball_y / Q);
                for (int r = 0; r < 6; ++r)
                    for (int c = 0; c < 8; ++c)
                        if ((g.bricks[r] & (1u << c)) && x0 + 2 > c * 16 && x0 < c * 16 + 15 &&
                            y0 + 2 > 10 + r * 4 && y0 < 13 + r * 4)
                            CHECK(0);
                if (g.ball_vy == 0) CHECK(0);
            }
        }
        CHECK(g.level >= 3);
        CHECK(g.lives == 3);
    }
}

static void test_breakout_random(void){
    uint32_t s = 99;
    int overs = 0;
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        FwBreakout g;
        fw_breakout_init(&g, W, H, seed);
        for (int t = 0; t < 100000; ++t) {
            FwGameInput in = random_input(&s, 1);
            FwGamePhase before = g.phase;
            fw_breakout_tick(&g, &in);
            if (g.phase == FW_GAME_OVER && before != FW_GAME_OVER) ++overs;
            if (g.paddle_x < 0 || g.paddle_x > (W - g.paddle_w) * Q || g.ball_x < 0 || g.ball_x > (W - 2) * Q ||
                g.speed > 2 * Q || g.lives < 0 || g.lives > 3)
                CHECK(0);
        }
    }
    CHECK(overs > 0);
}

static void test_breakout_draw(void){
    FwBreakout g;
    fw_breakout_init(&g, W, H, 5);
    g.score = 123;
    g.bricks[0] = (uint8_t)~1u;                                    /* column 0 of the top row gone */
    fw_breakout_draw(&g, &S);
    CHECK(!lit(0, 10) && is(16, 10, 255, 0, 0) && is(30, 12, 255, 0, 0) && !lit(31, 10) && !lit(16, 13));
    CHECK(is(0, 30, 160, 0, 255) && is(112, 14, 255, 120, 0));
    CHECK(is(0, 7, 50, 50, 50) && is(127, 7, 50, 50, 50));          /* ceiling line */
    CHECK(is(56, 61, 255, 255, 255) && is(71, 62, 255, 255, 255) && !lit(55, 61) && !lit(72, 61));   /* paddle */
    CHECK(lit_in(0, 0, 18, 7) > 0);                                 /* score */
    CHECK(is(125, 2, 150, 150, 150) && is(121, 2, 150, 150, 150) && is(117, 3, 150, 150, 150) && !lit(113, 2));   /* 3 lives */
    g.phase = FW_GAME_OVER;
    fw_breakout_draw(&g, &S);
    CHECK(lit_in(37, 32, 54, 8) > 20);
}

int main(void){
    test_pong_basics();
    test_pong_paddles();
    test_pong_hits();
    test_pong_points();
    test_pong_cpu();
    test_pong_soak();
    test_pong_replay();
    test_pong_draw();
    test_breakout_basics();
    test_breakout_bricks();
    test_breakout_paddle();
    test_breakout_lives_and_levels();
    test_breakout_soak();
    test_breakout_random();
    test_breakout_draw();
    printf("PASS: %d games checks\n", checks);
    return 0;
}
