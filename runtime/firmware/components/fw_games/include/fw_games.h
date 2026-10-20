#ifndef FW_GAMES_H
#define FW_GAMES_H
/* Pong and Breakout for the panel (OWNED-FIRMWARE-PLAN.md "Extras"). Not in
 * the stock firmware: these are owned additions, played from a phone over
 * Wi-Fi because the board has no buttons.
 *
 * Portable C with no SDK dependency, drawn through FwSurface. Each game is a
 * plain struct advanced by fixed ticks (FW_GAME_TICK_MS) with positions in
 * 1/256 px, so a seed and an input sequence always replay the same game. */
#include <stdbool.h>
#include <stdint.h>
#include "fw_text.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_GAME_TICK_MS 20      /* 50 ticks per second */
#define FW_GAME_NO_TARGET (-1)

/* One player's controls, as a phone sends them: hold a direction, or drag
 * to a position (the paddle's centre, in panel pixels along its axis). */
typedef struct {
    bool present;       /* a phone is playing this side (else the computer plays Pong's right side) */
    int8_t dir;         /* -1 up/left, 0 none, +1 down/right */
    int16_t target;     /* FW_GAME_NO_TARGET or a pixel position; overrides dir */
    bool action;        /* serve / start again */
} FwGamePlayer;

typedef struct {
    FwGamePlayer p[2];
} FwGameInput;

typedef enum {
    FW_GAME_READY,      /* waiting to serve */
    FW_GAME_PLAYING,
    FW_GAME_OVER,
} FwGamePhase;

/* ------------------------------------------------------------ Pong */

#define FW_PONG_WIN 11

typedef struct {
    int width, height;
    uint32_t rng;
    FwGamePhase phase;
    int32_t ball_x, ball_y, ball_vx, ball_vy;   /* 1/256 px, per tick */
    int32_t paddle_y[2];                        /* top edge, 1/256 px */
    uint8_t score[2];
    uint8_t serve_to;                           /* side the next serve goes to */
    int wait;                                   /* ticks until the automatic serve */
    uint32_t ticks;
    uint8_t winner;
    bool cpu[2];                                /* side played by the computer */
} FwPong;

#define FW_PONG_PADDLE_H 12
#define FW_PONG_PADDLE_W 2
#define FW_GAME_BALL 2
/* Left edge of each paddle. */
int fw_pong_paddle_x(const FwPong *g, int side);

void fw_pong_init(FwPong *g, int width, int height, uint32_t seed);
void fw_pong_tick(FwPong *g, const FwGameInput *in);
void fw_pong_draw(const FwPong *g, const FwSurface *s);

/* ------------------------------------------------------------ Breakout */

#define FW_BREAKOUT_COLS 8
#define FW_BREAKOUT_ROWS 6
#define FW_BREAKOUT_LIVES 3

typedef struct {
    int width, height;
    uint32_t rng;
    FwGamePhase phase;
    int32_t ball_x, ball_y, ball_vx, ball_vy;   /* 1/256 px, per tick */
    int32_t speed;                              /* ball speed, 1/256 px per tick */
    int32_t paddle_x;                           /* left edge, 1/256 px */
    int paddle_w;
    uint8_t bricks[FW_BREAKOUT_ROWS];           /* bit c = brick in column c */
    int bricks_left;
    int hits;                                   /* paddle hits this ball, for speed-ups */
    bool hit_top_rows, hit_ceiling;
    int32_t base_speed;                         /* speed of a new ball this level */
    uint32_t score;
    int lives, level;
    int wait;
    uint32_t ticks;
} FwBreakout;

void fw_breakout_init(FwBreakout *g, int width, int height, uint32_t seed);
void fw_breakout_tick(FwBreakout *g, const FwGameInput *in);   /* uses p[0] */
void fw_breakout_draw(const FwBreakout *g, const FwSurface *s);

/* Brick geometry: eight columns across the width, one pixel apart. */
int fw_breakout_brick_x(const FwBreakout *g, int col);
int fw_breakout_brick_y(int row);
int fw_breakout_brick_w(const FwBreakout *g);
#define FW_BREAKOUT_BRICK_H 3
#define FW_BREAKOUT_CEILING 8      /* rows above hold the score and lives */
#define FW_BREAKOUT_PADDLE_W 16
#define FW_BREAKOUT_PADDLE_W_SMALL 10

#ifdef __cplusplus
}
#endif
#endif
