#ifndef FW_ARCADE_H
#define FW_ARCADE_H
/* A games session played from phones (GAMES.md "Phone controller"): up to
 * two players, their input messages, the game clock and when the session
 * ends. Portable C; the board's server (main/games_server.c) feeds it
 * WebSocket messages and the panel task advances and draws it.
 *
 * Phone -> board messages (text, one per WebSocket frame):
 *   "G p" / "G b"  start Pong / Breakout       "X"  end the game
 *   "T <px>"       drag: paddle centre at px (-1 = finger lifted)
 *   "D <-1|0|1>"   held direction             "A"  serve / start again
 * Board -> phone: fw_arcade_status() JSON. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_games.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_ARCADE_PLAYERS 2
#define FW_ARCADE_IDLE_MS 60000      /* nobody touched a control: the game ends */
#define FW_ARCADE_EMPTY_MS 15000     /* no phone connected: the game ends */
#define FW_ARCADE_MAX_CATCHUP 10     /* game ticks run per advance at most */

typedef enum {
    FW_ARCADE_OFF,
    FW_ARCADE_PONG,
    FW_ARCADE_BREAKOUT,
} FwArcadeGame;

typedef struct {
    int width, height;
    FwArcadeGame game;
    FwPong pong;
    FwBreakout breakout;
    FwGamePlayer player[FW_ARCADE_PLAYERS];
    bool joined[FW_ARCADE_PLAYERS];
    uint32_t next_tick;          /* ms of the next game tick */
    uint32_t last_input;         /* ms of the last message from any player */
    uint32_t empty_since;        /* ms when the last phone left */
    uint32_t seed;
    uint32_t serial;             /* changes when something phones should see changes */
} FwArcade;

void fw_arcade_init(FwArcade *a, int width, int height, uint32_t seed);

/* A phone connected: its slot (0 = left / bottom player), or -1 when full. */
int fw_arcade_join(FwArcade *a, uint32_t now);
void fw_arcade_leave(FwArcade *a, int slot, uint32_t now);

/* One message from a player. Returns false (and changes nothing) when it is
 * not a valid message or the slot is not joined. */
bool fw_arcade_message(FwArcade *a, int slot, const char *msg, size_t len, uint32_t now);

/* Runs the game ticks due by `now` (at most FW_ARCADE_MAX_CATCHUP; a longer
 * stall skips ahead) and ends the session on the idle or empty timeouts.
 * Returns true while a game is on. */
bool fw_arcade_advance(FwArcade *a, uint32_t now);

bool fw_arcade_active(const FwArcade *a);
void fw_arcade_draw(const FwArcade *a, const FwSurface *s);

/* {"game":"pong","phase":"playing","you":0,"players":2,"score":[3,5]} or for
 * Breakout "score":120,"lives":3,"level":1; "game":"none" when off. Returns
 * the full length (like snprintf). */
size_t fw_arcade_status(const FwArcade *a, int slot, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
