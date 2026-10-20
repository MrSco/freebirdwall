/* Breakout: six rows of eight bricks, three balls. The ball speeds up after
 * the 4th and 12th paddle hit and on first reaching the top two rows; the
 * paddle shrinks once the ball reaches the ceiling, as in the arcade game.
 * Clearing the wall builds the next one, a little faster. */
#include <stdio.h>
#include "games_common.h"

#define BRICK_TOP 10                  /* first brick row */
#define BRICK_STEP_Y (FW_BREAKOUT_BRICK_H + 1)
#define PADDLE_H 2
#define DIR_SPEED (2 * Q)
#define DRAG_SPEED (4 * Q)
#define START_SPEED 230               /* ~0.9 px per tick */
#define LEVEL_SPEED_UP 20
#define SPEED_UP 40
#define MAX_SPEED (2 * Q)
#define SERVE_WAIT 75                 /* 1.5 s */

static const uint8_t POINTS[FW_BREAKOUT_ROWS] = {7, 7, 4, 4, 1, 1};
static const FwRgb ROW_COLOR[FW_BREAKOUT_ROWS] = {
    {255, 0, 0}, {255, 120, 0}, {255, 220, 0}, {0, 200, 0}, {0, 120, 255}, {160, 0, 255},
};
static const FwRgb WHITE = {255, 255, 255}, HUD = {150, 150, 150}, CEILING = {50, 50, 50}, BANNER = {255, 200, 0};

int fw_breakout_brick_w(const FwBreakout *g){ return g->width / FW_BREAKOUT_COLS - 1; }
int fw_breakout_brick_x(const FwBreakout *g, int col){ return col * (g->width / FW_BREAKOUT_COLS); }
int fw_breakout_brick_y(int row){ return BRICK_TOP + row * BRICK_STEP_Y; }

static int paddle_y(const FwBreakout *g){ return g->height - 3; }

static void build_wall(FwBreakout *g){
    for (int r = 0; r < FW_BREAKOUT_ROWS; ++r) g->bricks[r] = (uint8_t)((1u << FW_BREAKOUT_COLS) - 1);
    g->bricks_left = FW_BREAKOUT_ROWS * FW_BREAKOUT_COLS;
}

/* A new ball resting on the paddle. */
static void new_ball(FwBreakout *g){
    g->phase = FW_GAME_READY;
    g->wait = SERVE_WAIT;
    g->hits = 0;
    g->hit_top_rows = false;
    g->speed = g->base_speed;
    g->ball_vx = g->ball_vy = 0;
}

static void start_level(FwBreakout *g){
    build_wall(g);
    g->base_speed = START_SPEED + LEVEL_SPEED_UP * (g->level - 1);
    if (g->base_speed > MAX_SPEED) g->base_speed = MAX_SPEED;
    g->hit_ceiling = false;
    g->paddle_w = FW_BREAKOUT_PADDLE_W;
    new_ball(g);
}

void fw_breakout_init(FwBreakout *g, int width, int height, uint32_t seed){
    g->width = width;
    g->height = height;
    g->rng = seed ? seed : 1;
    g->score = 0;
    g->lives = FW_BREAKOUT_LIVES;
    g->level = 1;
    g->ticks = 0;
    g->paddle_x = (width - FW_BREAKOUT_PADDLE_W) / 2 * Q;
    start_level(g);
}

/* Velocity for the ball leaving the paddle: f in -Q..Q from the paddle's
 * left edge to its right edge. Always mostly upwards, so it cannot stall. */
static void aim(FwBreakout *g, int32_t f){
    g->ball_vx = g->speed * f / Q * 3 / 4;
    g->ball_vy = -(g->speed - abs32(g->ball_vx) / 2);
}

static void set_speed(FwBreakout *g, int32_t speed){
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    if (speed == g->speed) return;
    g->ball_vx = g->ball_vx * speed / g->speed;
    g->ball_vy = g->ball_vy * speed / g->speed;
    g->speed = speed;
}

static void rest_on_paddle(FwBreakout *g){
    g->ball_x = g->paddle_x + (g->paddle_w - FW_GAME_BALL) / 2 * Q;
    g->ball_y = (paddle_y(g) - FW_GAME_BALL) * Q;
}

/* Removes the first brick the ball overlaps; returns its row or -1. */
static int hit_brick(FwBreakout *g){
    int x0 = (int)(g->ball_x / Q), y0 = (int)(g->ball_y / Q);
    int bw = fw_breakout_brick_w(g);
    for (int r = 0; r < FW_BREAKOUT_ROWS; ++r) {
        int by = fw_breakout_brick_y(r);
        if (!g->bricks[r] || y0 + FW_GAME_BALL <= by || y0 >= by + FW_BREAKOUT_BRICK_H) continue;
        for (int c = 0; c < FW_BREAKOUT_COLS; ++c) {
            int bx = fw_breakout_brick_x(g, c);
            if (!(g->bricks[r] & (1u << c)) || x0 + FW_GAME_BALL <= bx || x0 >= bx + bw) continue;
            g->bricks[r] = (uint8_t)(g->bricks[r] & ~(1u << c));
            --g->bricks_left;
            g->score += POINTS[r];
            return r;
        }
    }
    return -1;
}

static void brick_hit(FwBreakout *g, int row){
    if (row <= 1 && !g->hit_top_rows) {
        g->hit_top_rows = true;
        set_speed(g, g->speed + SPEED_UP);
    }
}

static void paddle_hit(FwBreakout *g){
    int32_t f = (g->ball_x + FW_GAME_BALL * Q / 2 - g->paddle_x) * 2 * Q / (g->paddle_w * Q) - Q;   /* -Q..Q */
    g->ball_y = (paddle_y(g) - FW_GAME_BALL) * Q;
    ++g->hits;
    if (g->hits == 4 || g->hits == 12) set_speed(g, g->speed + SPEED_UP);
    aim(g, clamp32(f, -Q, Q));
}

static void lose_ball(FwBreakout *g){
    if (--g->lives <= 0) {
        g->lives = 0;
        g->phase = FW_GAME_OVER;
        return;
    }
    new_ball(g);
}

static void move_ball(FwBreakout *g){
    int32_t right = (g->width - FW_GAME_BALL) * Q, top = FW_BREAKOUT_CEILING * Q;
    int32_t py = paddle_y(g) * Q;
    int32_t steps = (abs32(g->ball_vx) > abs32(g->ball_vy) ? abs32(g->ball_vx) : abs32(g->ball_vy)) / Q + 1;
    for (int32_t i = 0; i < steps && g->phase == FW_GAME_PLAYING; ++i) {
        int32_t dx = g->ball_vx / steps, dy = g->ball_vy / steps;
        int row;
        /* one axis at a time, so the side of a brick that was hit is known */
        g->ball_x += dx;
        if (g->ball_x < 0) { g->ball_x = -g->ball_x; g->ball_vx = -g->ball_vx; }
        if (g->ball_x > right) { g->ball_x = 2 * right - g->ball_x; g->ball_vx = -g->ball_vx; }
        if ((row = hit_brick(g)) >= 0) {
            g->ball_x -= dx;
            g->ball_vx = -g->ball_vx;
            brick_hit(g, row);
        }
        g->ball_y += dy;
        if (g->ball_y < top) {
            g->ball_y = 2 * top - g->ball_y;
            g->ball_vy = -g->ball_vy;
            if (!g->hit_ceiling) {
                g->hit_ceiling = true;
                g->paddle_w = FW_BREAKOUT_PADDLE_W_SMALL;
            }
        }
        if ((row = hit_brick(g)) >= 0) {
            g->ball_y -= dy;
            g->ball_vy = -g->ball_vy;
            brick_hit(g, row);
        }
        if (g->bricks_left == 0) {
            ++g->level;
            start_level(g);
            return;
        }
        if (g->ball_vy > 0 && g->ball_y + FW_GAME_BALL * Q > py && g->ball_y < py + PADDLE_H * Q &&
            g->ball_x + FW_GAME_BALL * Q > g->paddle_x && g->ball_x < g->paddle_x + g->paddle_w * Q)
            paddle_hit(g);
        if (g->ball_y >= g->height * Q) lose_ball(g);
    }
}

void fw_breakout_tick(FwBreakout *g, const FwGameInput *in){
    const FwGamePlayer *p = &in->p[0];
    ++g->ticks;
    if (g->phase != FW_GAME_OVER)
        g->paddle_x = clamp32(game_steer(g->paddle_x, p, g->paddle_w / 2, DIR_SPEED, DRAG_SPEED), 0,
                              (g->width - g->paddle_w) * Q);
    switch (g->phase) {
    case FW_GAME_READY:
        rest_on_paddle(g);
        if (p->action || --g->wait <= 0) {
            g->phase = FW_GAME_PLAYING;
            aim(g, game_range(&g->rng, -Q / 2, Q / 2));
        }
        break;
    case FW_GAME_PLAYING:
        move_ball(g);
        break;
    case FW_GAME_OVER:
        if (p->action) {
            uint32_t rng = g->rng;
            fw_breakout_init(g, g->width, g->height, rng);
        }
        break;
    }
}

void fw_breakout_draw(const FwBreakout *g, const FwSurface *s){
    char text[16];
    int bw = fw_breakout_brick_w(g);
    fw_clear(s);
    snprintf(text, sizeof text, "%u", (unsigned)g->score);
    fw_text_draw(s, 0, 0, text, HUD, 1);
    for (int i = 0; i < g->lives; ++i) fw_fill_rect(s, g->width - 3 - 4 * i, 2, 2, 2, HUD);
    fw_hline(s, 0, FW_BREAKOUT_CEILING - 1, g->width, CEILING);
    for (int r = 0; r < FW_BREAKOUT_ROWS; ++r)
        for (int c = 0; c < FW_BREAKOUT_COLS; ++c)
            if (g->bricks[r] & (1u << c))
                fw_fill_rect(s, fw_breakout_brick_x(g, c), fw_breakout_brick_y(r), bw, FW_BREAKOUT_BRICK_H, ROW_COLOR[r]);
    fw_fill_rect(s, (int)(g->paddle_x / Q), paddle_y(g), g->paddle_w, PADDLE_H, WHITE);
    if (g->phase == FW_GAME_OVER) {
        static const char over[] = "GAME OVER";
        fw_text_draw_bg(s, (g->width - (int)(sizeof over - 1) * FW_FONT_ADVANCE) / 2, g->height / 2, over, BANNER,
                        (FwRgb){0, 0, 0}, 1);
    } else {
        fw_fill_rect(s, (int)(g->ball_x / Q), (int)(g->ball_y / Q), FW_GAME_BALL, FW_GAME_BALL, WHITE);
    }
}
