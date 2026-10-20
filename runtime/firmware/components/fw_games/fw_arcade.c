/* A games session played from phones; see fw_arcade.h. */
#include "fw_arcade.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void release(FwGamePlayer *p){
    p->dir = 0;
    p->target = FW_GAME_NO_TARGET;
    p->action = false;
}

void fw_arcade_init(FwArcade *a, int width, int height, uint32_t seed){
    memset(a, 0, sizeof *a);
    a->width = width;
    a->height = height;
    a->seed = seed ? seed : 1;
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i) release(&a->player[i]);
}

bool fw_arcade_active(const FwArcade *a){ return a->game != FW_ARCADE_OFF; }

static int players(const FwArcade *a){
    int n = 0;
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i) n += a->joined[i];
    return n;
}

int fw_arcade_join(FwArcade *a, uint32_t now){
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i)
        if (!a->joined[i]) {
            a->joined[i] = true;
            a->player[i].present = true;
            release(&a->player[i]);
            a->last_input = now;
            ++a->serial;
            return i;
        }
    return -1;
}

void fw_arcade_leave(FwArcade *a, int slot, uint32_t now){
    if (slot < 0 || slot >= FW_ARCADE_PLAYERS || !a->joined[slot]) return;
    a->joined[slot] = false;
    a->player[slot].present = false;
    release(&a->player[slot]);
    if (!players(a)) a->empty_since = now;
    ++a->serial;
}

static void start(FwArcade *a, FwArcadeGame game, uint32_t now){
    a->seed = a->seed * 1664525u + 1013904223u + now;
    if (game == FW_ARCADE_PONG) fw_pong_init(&a->pong, a->width, a->height, a->seed);
    else fw_breakout_init(&a->breakout, a->width, a->height, a->seed);
    a->game = game;
    a->next_tick = now;
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i) release(&a->player[i]);
    ++a->serial;
}

/* "T -12", "D 1": an integer after one space, nothing else. */
static bool number(const char *s, size_t len, long lo, long hi, long *out){
    char buf[12], *end;
    if (len < 3 || len - 2 >= sizeof buf || s[1] != ' ') return false;
    memcpy(buf, s + 2, len - 2);
    buf[len - 2] = 0;
    *out = strtol(buf, &end, 10);
    return end != buf && !*end && *out >= lo && *out <= hi;
}

bool fw_arcade_message(FwArcade *a, int slot, const char *msg, size_t len, uint32_t now){
    FwGamePlayer *p;
    long v;
    if (slot < 0 || slot >= FW_ARCADE_PLAYERS || !a->joined[slot] || !msg || !len) return false;
    p = &a->player[slot];
    switch (msg[0]) {
    case 'G':
        if (len != 3 || msg[1] != ' ' || (msg[2] != 'p' && msg[2] != 'b')) return false;
        start(a, msg[2] == 'p' ? FW_ARCADE_PONG : FW_ARCADE_BREAKOUT, now);
        break;
    case 'X':
        if (len != 1) return false;
        a->game = FW_ARCADE_OFF;
        ++a->serial;
        break;
    case 'T':
        if (!number(msg, len, -1, 1000, &v)) return false;
        p->target = (int16_t)(v < 0 ? FW_GAME_NO_TARGET : v);
        break;
    case 'D':
        if (!number(msg, len, -1, 1, &v)) return false;
        p->dir = (int8_t)v;
        break;
    case 'A':
        if (len != 1) return false;
        p->action = true;
        break;
    default:
        return false;
    }
    a->last_input = now;
    return true;
}

static FwGamePhase phase(const FwArcade *a){
    return a->game == FW_ARCADE_PONG ? a->pong.phase : a->breakout.phase;
}

static void tick(FwArcade *a){
    FwGameInput in;
    FwGamePhase before = phase(a);
    memset(&in, 0, sizeof in);
    if (a->game == FW_ARCADE_PONG) {
        uint8_t s0 = a->pong.score[0], s1 = a->pong.score[1];
        in.p[0] = a->player[0];
        in.p[1] = a->player[1];
        fw_pong_tick(&a->pong, &in);
        if (s0 != a->pong.score[0] || s1 != a->pong.score[1]) ++a->serial;
    } else {
        uint32_t score = a->breakout.score;
        int lives = a->breakout.lives;
        /* one paddle: the first phone that is playing */
        in.p[0] = a->joined[0] || !a->joined[1] ? a->player[0] : a->player[1];
        in.p[1].target = FW_GAME_NO_TARGET;
        fw_breakout_tick(&a->breakout, &in);
        if (score != a->breakout.score || lives != a->breakout.lives) ++a->serial;
    }
    if (phase(a) != before) ++a->serial;
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i) a->player[i].action = false;   /* one-shot */
}

bool fw_arcade_advance(FwArcade *a, uint32_t now){
    int ran = 0;
    if (a->game == FW_ARCADE_OFF) return false;
    if (now - a->last_input >= FW_ARCADE_IDLE_MS || (!players(a) && now - a->empty_since >= FW_ARCADE_EMPTY_MS)) {
        a->game = FW_ARCADE_OFF;
        ++a->serial;
        return false;
    }
    while ((int32_t)(now - a->next_tick) >= 0) {
        if (ran == FW_ARCADE_MAX_CATCHUP) {          /* stalled: skip ahead, do not race */
            a->next_tick = now + FW_GAME_TICK_MS;
            break;
        }
        tick(a);
        a->next_tick += FW_GAME_TICK_MS;
        ++ran;
    }
    return true;
}

void fw_arcade_draw(const FwArcade *a, const FwSurface *s){
    if (a->game == FW_ARCADE_PONG) fw_pong_draw(&a->pong, s);
    else if (a->game == FW_ARCADE_BREAKOUT) fw_breakout_draw(&a->breakout, s);
}

size_t fw_arcade_status(const FwArcade *a, int slot, char *buf, size_t cap){
    static const char *const phases[] = {"ready", "playing", "over"};
    int n;
    if (a->game == FW_ARCADE_OFF)
        n = snprintf(buf, cap, "{\"game\":\"none\",\"you\":%d,\"players\":%d}", slot, players(a));
    else if (a->game == FW_ARCADE_PONG)
        n = snprintf(buf, cap, "{\"game\":\"pong\",\"phase\":\"%s\",\"you\":%d,\"players\":%d,\"score\":[%u,%u]}",
                     phases[a->pong.phase], slot, players(a), (unsigned)a->pong.score[0], (unsigned)a->pong.score[1]);
    else
        n = snprintf(buf, cap,
                     "{\"game\":\"breakout\",\"phase\":\"%s\",\"you\":%d,\"players\":%d,\"score\":%lu,\"lives\":%d,"
                     "\"level\":%d}",
                     phases[a->breakout.phase], slot, players(a), (unsigned long)a->breakout.score, a->breakout.lives,
                     a->breakout.level);
    return n < 0 ? 0 : (size_t)n;
}
