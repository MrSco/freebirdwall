# Games — Pong and Breakout

Owned additions (OWNED-FIRMWARE-PLAN.md "Extras"); the stock firmware has no
games. Both are portable C in `components/fw_games`, drawn through
`FwSurface` on the 128×64 panel, advanced in fixed 20 ms ticks with positions
in 1/256 px, so a seed and an input sequence always replay the same game.
The board has no buttons: a phone plays over Wi-Fi by sending an
`FwGamePlayer` per side — a held direction, or a dragged position (the
paddle's centre in pixels), plus an action for serve / start again. The phone
page and the board loop are not built yet.

## Pong (`fw_pong.c`)

- Paddles 2×12 px, 3 px from each edge; ball 2×2; a dashed net (2 on, 2 off)
  in grey; scores at the top of each half; first to 11.
- Held direction 1.5 px/tick; dragged up to 3 px/tick. A side with no phone is
  played by the computer: 1.25 px/tick, follows the ball only while it comes
  its way and is past the net, else drifts to the middle — beatable.
- Serve after 2 s at the start and 1 s after a point (or at once on action),
  1 px/tick across towards the side that lost the point, random slope within
  ±0.75. Each paddle hit adds 1/16 px/tick (max 3); the slope follows where
  the ball met the paddle (edge = 3/4 of the speed).
- Game over shows `P1 WINS`, `P2 WINS` or `CPU WINS` until a player acts.

## Breakout (`fw_breakout.c`)

- Score and three lives (dots) above a ceiling line at y 7; six rows of eight
  bricks, 15×3 px, 1 px apart, from y 10 (red, orange, yellow, green, blue,
  purple; 7, 7, 4, 4, 1, 1 points); paddle 16×2 at y 61.
- Arcade rules: the ball speeds up after the 4th and 12th paddle hit and on
  first reaching the top two rows; the paddle shrinks to 10 px once the ball
  reaches the ceiling. Clearing the wall builds the next one, 20/256 px/tick
  faster (max 2 px/tick). The ball leaves the paddle mostly upwards, angled by
  where it landed, so it never stalls sideways.
- Collisions move one axis at a time in sub-steps of at most 1 px, so the ball
  never passes through a brick or the paddle.

## Phone controller

- **Finding it.** The controller is at `http://<board address>:8080/play`.
  The owned dashboard (`:8080/`, DASHBOARD.md) has Play Pong / Play
  Breakout buttons that open it with the game started, a "Show QR on the
  wall (30 s)" button, and a switch (off by default) that shows the
  "Scan to play" QR (`fw_screen_play`, drawn like the setup QR) for 15 s every
  2 minutes with live flights, or as a 15 s turn among the demo screens. Once a phone has the page it can be added to its home
  screen.
- **The page** (`main/games_page.html`, embedded in the firmware, ~6 KB, no
  external files): pick Pong or Breakout, drag on the big pad (up/down for
  Pong, left/right for Breakout), Serve, hold buttons as an alternative, End
  game. The score, phase and your side update live; the phone buzzes on a
  score change where supported. It reconnects by itself.
- **Protocol** (one WebSocket text frame each, `/ws`): `G p` / `G b` start
  Pong / Breakout, `X` end, `T <px>` drag target (paddle centre in panel
  pixels, `-1` none), `D <-1|0|1>` held direction, `A` serve / start again.
  The first drag position goes out at once, then at most one every 30 ms (the
  last always goes); lifting the finger sends nothing, so the paddle finishes
  its move. The board answers with status JSON whenever the score, phase or
  players change, and once a second.
- **Session** (`components/fw_games/fw_arcade.c`, portable): two player slots
  (the first phone is the left Pong paddle and the Breakout player; a third
  and fourth phone can watch), fixed 20 ms ticks with bounded catch-up, a
  one-tick serve action, and the game ends after 60 s without input or 15 s
  with no phone connected. One phone plays Pong against the computer.
- **On the panel** (`main/display.cpp`, `main/games_server.c`): while a game
  is on it replaces the normal screens (Wi-Fi problems still take over). The
  panel task advances the session every 20 ms and draws it into an offscreen
  RGB565 canvas (`fw_canvas`, 2 × 16 KiB, only while playing); only changed
  pixels reach the panel, so the single-buffered panel does not flicker.
- **Who can play.** Anyone on the home network can open the page; the server
  carries only the page and game input — no settings, credentials or storage.

## Verification

`tests/test_games.c` (run by `tests/run_host_tests.py`): serve timing, paddle
movement and clamping, hit angles and the speed cap, walls, scoring and game
over, the computer player, exact brick/paddle/ceiling behaviour, lives and
levels, pixel checks of both screens, a replay check, ~700 000 ticks of
random play with invariants (ball inside the court and never inside a brick,
counts consistent), and an automatic player that clears two walls without
losing a ball. `tests/test_arcade.c`: slots, every message form and its
rejects, tick timing and catch-up, the one-shot serve, the computer taking
an empty side, input routing for Breakout, status JSON, both timeouts, and
the canvas (full first flush, change-only flushes, runs, invalidation). The
page was exercised in a browser against a stand-in server speaking the same
protocol.

## On the board — 2026-10-19

`owned-skeleton-v1-5bdf3f03ffec` (pending into app0, rolled back to
`fb336e55529a…` in app1 afterwards). The games server was up 5.8 s after
boot; the controller page answered from the desk (200, 7 050 bytes). The
owner played from a phone on the home network: a phone joined at 111 s, the
game started at 115 s (canvas allocated, 83 796 bytes free), Pong and then
Breakout were played, the session ended at 206 s and the phone left at 246 s.
Owner's verdict: both games work, the paddle feels smooth, no flicker. No
crash in 570 s; heap low-water mark 59 496 bytes (during the start-up network
probe, before any game); panel task stack margin 2 432. Stock came back
confirmed on the same IP with firmware status, aliases and settings
unchanged.
