# Owned dashboard

The owned firmware's own web app on the home network, at
`http://<wall address>:8080/` (port 8080 keeps it apart from the setup server
on 80). It starts once the wall is online (`main/games_server.c`) and is a
single self-contained page (`main/dashboard.html`, embedded in the firmware,
no external files). Signed in, it has every setting (Display, Clock, Flights,
Airports, Games, Device; see [SETTINGS.md](SETTINGS.md)). Without signing in
it shows only **Games**:

- **Show "Scan to play" between screens** — off by default. When on, the
  "Scan to play" QR to the controller shows for 15 s every 2 minutes while
  live flights are up (then the flights continue where they were), and gets a
  15 s turn of its own among the demo screens.
- **Play Pong / Play Breakout** — open the controller (`/play?g=p|b`), which
  starts that game as soon as it connects.
- **Show QR on the wall (30 s)** — the wall shows "Scan to play" at once for
  30 s, whatever the switch says (Wi-Fi problems and a running game still
  come first).
- A status line: the game on and how many phones are playing; refreshed every
  5 s.

## API

`GET /api/games` → `{"in_rotation":false,"qr_showing":false,"game":"none","players":0}`.
`POST /api/games` with `{"in_rotation":true|false}` and/or `{"show_qr":true}`
(at most 128 bytes) → the same JSON; 400 for anything else, 500 when the
setting could not be saved.

## Settings storage

Owned-only settings live in their own NVS namespace, **`fw_owned`**
(`main/owned_settings.c` for `games_rot`, u8; the settings and sign-ins are
in SETTINGS.md). The firmware
writes it only when the value actually changes, and never touches stock's
namespaces. Stock never reads `fw_owned`, so the key is harmless after
switching back to stock (it stays until NVS is erased). This is the owned
firmware's second NVS write, next to saving Wi-Fi credentials in setup mode.

## Who can use it

Anyone on the home network can use the Games card, like the controller: it
changes only the games switch and the QR request. Everything else needs a
browser signed in with a code shown on the wall (SETTINGS.md, "Signing in").

## On the board — 2026-10-19

Test build `owned-skeleton-v1-95c5883876c1`: `/` and `/play` served (200);
`GET /api/games` gave the default state (switch off); `show_qr` put the QR on
the wall and `qr_showing` cleared itself after 30 s; an unknown body was
refused (400); the switch on, on again, then off wrote NVS exactly twice
(`owned_setting_saved` ×2, the repeat wrote nothing). The `fw_owned`
namespace now exists on the board with the switch off.
