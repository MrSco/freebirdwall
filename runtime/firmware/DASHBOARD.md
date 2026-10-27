# Owned dashboard

The owned firmware's own web app on the home network, at
`http://<wall address>:8080/` (port 8080 keeps it apart from the setup server
on 80). It starts once the wall is online (`main/games_server.c`) and is a
single page (`main/dashboard.html`, embedded in the firmware; the only
outside files are the areas map's, below). Signed in, it has every setting (Display, Clock, Flights,
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

## Preview

The Display, Clock and Airports tabs open with a picture of the wall, drawn
LED by LED, that follows every edit before it is saved. It is the wall's own
screen code, built for the browser: `preview/fw_preview.c` runs the live
screens as `main/display.cpp`'s `live_step` does (rotation, tracked flights,
the loading template, the clock), with the screen, text, clock, flight, logo
and settings sources the firmware uses, compiled by Zig to
`main/preview.wasm` (`py tools/build_preview.py`; the file is committed, so a
firmware build needs no WebAssembly compiler). `tests/test_preview.py` builds
the same sources for the desktop and checks every frame of a set of scenarios
(layouts, title, units, border, loading templates, clock, aliases, too many
flights, tracked flights, scrolling, logos, a new list mid-rotation) is
byte-identical to the browser build's; it also checks the committed file is a
fresh build. Built as the board builds them: `char` unsigned, 32-bit `long`.

What the page supplies instead of the board: the time, the settings being
edited, the airport names being edited, the wall's flights now
(`GET /api/flights`, every 20 s; three sample flights when there are none)
and the logos the wall would draw for them (`GET /api/logo`). Display has a
**Flights / No flights** switch for the loading template; Clock shows the
clock. Brightness and sleep are not in the pixels on the board either: the
picture is dimmed to hint at them, and the note under it says which.

## Areas map

**Flights › Areas to watch** shows the areas as circles on an OpenStreetMap
map, with collapsed cards below (name, radius and altitudes at a glance). An
open area is edited on the map: tap (also inside its circle) or drag its pin
to move it; the radius slider is logarithmic (1–250 km) and shows the
distance unit chosen under Display, altitudes the altitude unit. **Search**
asks OpenStreetMap's Nominatim (one request per search, no autocomplete) and
moves the open area to the place picked, or adds an area there. Coordinates
can still be typed under **Coordinates**.

Leaflet 1.9.4 (unpkg, pinned, with integrity hashes), the map pictures
(tile.openstreetmap.org, with attribution) and the search come over the
phone's own internet, only once the Flights tab opens; the wall never
contacts them, and they carry no keys (only same-origin calls do). Without
internet the map says so and the coordinates stay editable.

## Settings storage

Owned-only settings live in their own NVS namespace, **`fw_owned`**
(`main/owned_settings.c` for `games_rot`, u8; the settings and sign-ins are
in SETTINGS.md). The firmware
writes it only when the value actually changes, and never touches stock's
namespaces. Stock never reads `fw_owned`, so the key is harmless after
switching back to stock (it stays until NVS is erased). This is the owned
firmware's second NVS write, next to saving Wi-Fi credentials in setup mode.

## Preview routes

- `GET /preview.wasm`: the preview (about 130 KB from flash), `no-cache` with
  the image's CRC as ETag, so it is downloaded again only after an update.
- `GET /api/flights` (signed in): `{"state":n,"tracked":b,"overflow":n,
  "flights":[...]}`, the feed's last good response, each flight with
  `FwFlight`'s fields by name, empty and unknown ones left out (`prog` with the
  progress bar's value). Copied one flight at a time (`fw_feed_flight`) and
  sent in pieces through a 1 KiB buffer on the stack: no heap, since the page
  asks every 20 s and the wall may be in the middle of a TLS request.
- `GET /api/logo?al=<airline>&ac=<aircraft class>` (signed in): the PNG the
  wall would draw (`fw_logo_resolve` on the asset partition, LOGOS.md), cached
  a day by the browser; 404 when there is none. It allocates the file's own
  size (most logos are under 1 KB). The asset partition now takes
  a mutex on every call (`fw_assets.c`), since the display, the community
  worker and this route all read it.

- `GET /api/memory` (signed in): `{"free","min_free","largest"}`, internal
  RAM now, its low-water mark since start and the largest free block; the
  Device tab shows it.

The dashboard server has room for 32 routes (was 24).

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
