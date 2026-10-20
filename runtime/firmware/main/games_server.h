#ifndef FLIGHTWALL_GAMES_SERVER_H
#define FLIGHTWALL_GAMES_SERVER_H
/* The owned dashboard and the games controller on the home network
 * (DASHBOARD.md, GAMES.md "Phone controller"): pages and a WebSocket on port
 * 8080. The session itself is
 * components/fw_games fw_arcade; the panel task advances and draws it. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_text.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_GAMES_PORT 8080

/* Starts the server (once the board is online). */
bool fw_games_start(void);
bool fw_games_running(void);

/* "http://<station address>:8080/play", for the "Scan to play" screen. */
bool fw_games_url(char *buf, size_t cap);

/* The dashboard's games switch: "Scan to play" between the normal screens. */
bool fw_games_in_rotation(void);

/* Non-zero while the dashboard's "Show QR on the wall" is in effect (30 s);
 * the value changes with each new request. */
uint32_t fw_games_qr_request(uint32_t now_ms);

/* Which QR the current request is for. */
typedef enum { FW_QR_PLAY, FW_QR_DASHBOARD } FwQrKind;
FwQrKind fw_games_qr_kind(void);

/* The dashboard's address, "http://<wall address>:8080/" (false until the
 * server runs). */
bool fw_dashboard_url(char *buf, size_t cap);

/* Shows the dashboard's address as a QR for ms (README "Find the dashboard"):
 * at start-up while no browser is signed in, so a new owner can reach it. */
void fw_dashboard_hint(uint32_t ms);

/* True while a game is on (the panel shows it instead of the normal screens). */
bool fw_games_active(void);

/* Panel task: runs the game ticks due by now_ms and draws the game. Returns
 * false once the game has ended. */
bool fw_games_frame(const FwSurface *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
