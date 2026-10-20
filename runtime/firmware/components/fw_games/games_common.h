/* Shared helpers for the games (internal). */
#ifndef FW_GAMES_COMMON_H
#define FW_GAMES_COMMON_H
#include <stdint.h>
#include "fw_games.h"

#define Q 256   /* fixed point: 1/256 px */

static inline uint32_t game_rand(uint32_t *s){   /* xorshift32 */
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* Uniform in [lo, hi]. */
static inline int32_t game_range(uint32_t *s, int32_t lo, int32_t hi){
    return lo + (int32_t)(game_rand(s) % (uint32_t)(hi - lo + 1));
}

static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int32_t abs32(int32_t v){ return v < 0 ? -v : v; }

/* Moves pos (1/256 px) towards a player's input: a dragged target at up to
 * drag_speed per tick, else the held direction at dir_speed. centre_offset
 * turns the target (the paddle's centre) into the paddle's edge. */
static inline int32_t game_steer(int32_t pos, const FwGamePlayer *p, int centre_offset,
                                 int32_t dir_speed, int32_t drag_speed){
    if (p->target != FW_GAME_NO_TARGET) {
        int32_t want = ((int32_t)p->target - centre_offset) * Q;
        return pos + clamp32(want - pos, -drag_speed, drag_speed);
    }
    return pos + p->dir * dir_speed;
}

#endif
