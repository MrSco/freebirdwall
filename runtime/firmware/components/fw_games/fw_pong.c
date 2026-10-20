/* Pong: two paddles, first to FW_PONG_WIN. A side with no phone is played by
 * the computer, which is fast but late, so it can be beaten. */
#include <stdio.h>
#include "games_common.h"

#define PADDLE_GAP 3                  /* px between the edge and a paddle */
#define DIR_SPEED (3 * Q / 2)         /* held direction: 1.5 px per tick */
#define DRAG_SPEED (3 * Q)            /* dragged: up to 3 px per tick */
#define CPU_SPEED (5 * Q / 4)         /* computer: 1.25 px per tick */
#define SERVE_SPEED Q                 /* 1 px per tick across */
#define MAX_SPEED (3 * Q)
#define SPEED_UP (Q / 16)             /* per paddle hit */
#define SERVE_WAIT 50                 /* 1 s */
#define START_WAIT 100                /* 2 s */

static const FwRgb WHITE = {255, 255, 255}, NET = {70, 70, 70}, SCORE = {150, 150, 150}, BANNER = {255, 200, 0};

int fw_pong_paddle_x(const FwPong *g, int side){
    return side == 0 ? PADDLE_GAP : g->width - PADDLE_GAP - FW_PONG_PADDLE_W;
}

static void place_ball(FwPong *g){
    g->ball_x = (g->width / 2 - FW_GAME_BALL / 2) * Q;
    g->ball_y = (g->height / 2 - FW_GAME_BALL / 2) * Q;
    g->ball_vx = g->ball_vy = 0;
}

void fw_pong_init(FwPong *g, int width, int height, uint32_t seed){
    g->width = width;
    g->height = height;
    g->rng = seed ? seed : 1;
    g->phase = FW_GAME_READY;
    g->score[0] = g->score[1] = 0;
    g->paddle_y[0] = g->paddle_y[1] = (height - FW_PONG_PADDLE_H) / 2 * Q;
    g->serve_to = (uint8_t)(game_rand(&g->rng) & 1);
    g->wait = START_WAIT;
    g->ticks = 0;
    g->winner = 0;
    g->cpu[0] = false;
    g->cpu[1] = true;
    place_ball(g);
}

static void serve(FwPong *g){
    g->ball_vx = g->serve_to ? SERVE_SPEED : -SERVE_SPEED;
    g->ball_vy = game_range(&g->rng, -3 * Q / 4, 3 * Q / 4);
    g->phase = FW_GAME_PLAYING;
}

/* The computer follows the ball while it comes its way and is past the net,
 * and drifts back to the middle otherwise. */
static int32_t cpu_move(const FwPong *g, int side){
    int32_t centre = g->paddle_y[side] + FW_PONG_PADDLE_H * Q / 2;
    int32_t want = (g->height / 2) * Q;
    bool coming = side ? g->ball_vx > 0 : g->ball_vx < 0;
    bool past_net = side ? g->ball_x > g->width * Q / 2 : g->ball_x < g->width * Q / 2;
    if (g->phase == FW_GAME_PLAYING && coming && past_net) want = g->ball_y + FW_GAME_BALL * Q / 2;
    return g->paddle_y[side] + clamp32(want - centre, -CPU_SPEED, CPU_SPEED);
}

static void move_paddles(FwPong *g, const FwGameInput *in){
    for (int side = 0; side < 2; ++side) {
        const FwGamePlayer *p = &in->p[side];
        int32_t y;
        g->cpu[side] = !p->present;
        y = p->present ? game_steer(g->paddle_y[side], p, FW_PONG_PADDLE_H / 2, DIR_SPEED, DRAG_SPEED)
                       : cpu_move(g, side);
        g->paddle_y[side] = clamp32(y, 0, (g->height - FW_PONG_PADDLE_H) * Q);
    }
}

/* Bounce off a paddle: faster across, and the further from the paddle's
 * centre the ball lands, the steeper it leaves. */
static void hit(FwPong *g, int side){
    int32_t speed = abs32(g->ball_vx) + SPEED_UP;
    int32_t half = FW_PONG_PADDLE_H * Q / 2;
    int32_t offset = g->ball_y + FW_GAME_BALL * Q / 2 - (g->paddle_y[side] + half);   /* -half..half */
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    g->ball_vx = side ? -speed : speed;
    g->ball_vy = clamp32(offset, -half, half) * speed / half * 3 / 4;
    g->ball_x = side ? (fw_pong_paddle_x(g, 1) - FW_GAME_BALL) * Q : (fw_pong_paddle_x(g, 0) + FW_PONG_PADDLE_W) * Q;
}

static bool overlaps_paddle(const FwPong *g, int side){
    int32_t px = fw_pong_paddle_x(g, side) * Q;
    return g->ball_x + FW_GAME_BALL * Q > px && g->ball_x < px + FW_PONG_PADDLE_W * Q &&
           g->ball_y + FW_GAME_BALL * Q > g->paddle_y[side] && g->ball_y < g->paddle_y[side] + FW_PONG_PADDLE_H * Q;
}

static void point(FwPong *g, int scorer){
    ++g->score[scorer];
    g->serve_to = (uint8_t)!scorer;           /* the next serve goes to the side that lost the point */
    place_ball(g);
    if (g->score[scorer] >= FW_PONG_WIN) {
        g->phase = FW_GAME_OVER;
        g->winner = (uint8_t)scorer;
    } else {
        g->phase = FW_GAME_READY;
        g->wait = SERVE_WAIT;
    }
}

static void move_ball(FwPong *g){
    int32_t bottom = (g->height - FW_GAME_BALL) * Q;
    int32_t steps = (abs32(g->ball_vx) > abs32(g->ball_vy) ? abs32(g->ball_vx) : abs32(g->ball_vy)) / Q + 1;
    for (int32_t i = 0; i < steps && g->phase == FW_GAME_PLAYING; ++i) {
        g->ball_x += g->ball_vx / steps;
        g->ball_y += g->ball_vy / steps;
        if (g->ball_y < 0) { g->ball_y = -g->ball_y; g->ball_vy = -g->ball_vy; }
        if (g->ball_y > bottom) { g->ball_y = 2 * bottom - g->ball_y; g->ball_vy = -g->ball_vy; }
        if (g->ball_vx < 0 && overlaps_paddle(g, 0)) hit(g, 0);
        else if (g->ball_vx > 0 && overlaps_paddle(g, 1)) hit(g, 1);
        if (g->ball_x + FW_GAME_BALL * Q <= 0) point(g, 1);
        else if (g->ball_x >= g->width * Q) point(g, 0);
    }
}

void fw_pong_tick(FwPong *g, const FwGameInput *in){
    bool action = in->p[0].action || in->p[1].action;
    ++g->ticks;
    move_paddles(g, in);
    switch (g->phase) {
    case FW_GAME_READY:
        if (action || --g->wait <= 0) serve(g);
        break;
    case FW_GAME_PLAYING:
        move_ball(g);
        break;
    case FW_GAME_OVER:
        if (action) {
            uint32_t rng = g->rng;
            fw_pong_init(g, g->width, g->height, rng);
        }
        break;
    }
}

void fw_pong_draw(const FwPong *g, const FwSurface *s){
    char text[16];
    fw_clear(s);
    for (int y = 0; y < g->height; y += 4) fw_vline(s, g->width / 2, y, 2, NET);
    for (int side = 0; side < 2; ++side) {
        int len = snprintf(text, sizeof text, "%u", (unsigned)g->score[side]);
        int centre = side ? g->width * 3 / 4 : g->width / 4;
        fw_text_draw(s, centre - len * FW_FONT_ADVANCE / 2, 1, text, SCORE, 1);
        fw_fill_rect(s, fw_pong_paddle_x(g, side), (int)(g->paddle_y[side] / Q), FW_PONG_PADDLE_W, FW_PONG_PADDLE_H, WHITE);
    }
    if (g->phase == FW_GAME_OVER) {
        const char *who = g->cpu[g->winner] ? "CPU WINS" : g->winner ? "P2 WINS" : "P1 WINS";
        int len = snprintf(text, sizeof text, "%s", who);
        fw_text_draw_bg(s, (g->width - len * FW_FONT_ADVANCE) / 2, g->height / 2 - FW_FONT_LINE / 2, text, BANNER,
                        (FwRgb){0, 0, 0}, 1);
    } else {
        fw_fill_rect(s, (int)(g->ball_x / Q), (int)(g->ball_y / Q), FW_GAME_BALL, FW_GAME_BALL, WHITE);
    }
}
