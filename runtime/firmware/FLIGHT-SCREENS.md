# Flight screens — recovered from the vendor firmware

The ordinary flight card, the tracked-flight card and the text they show,
recovered from vendor app `0ab195d6…` (Ghidra 12.1.4 headless, whole-program
decompile, objdump of IROM for details Ghidra loses). Addresses are functions
in that image. Companion documents: [LOGOS.md](LOGOS.md), [CLOCK.md](CLOCK.md),
[vendor-layouts.json](vendor-layouts.json).

## Display modes — `FUN_42015fb8`

The display loop picks one mode per pass: 0 ordinary flight (`FUN_42009bfc` →
`FUN_420097a4`), 1 tracked flight (`FUN_4200c290`), 2/3 text or loading template
(`FUN_42013fe0`: `CLOCK`, `ELLIPSIS`, `BLANK`, else the text), 6 "too many
flights" (`FUN_4200959c`), 7 upcoming list (`FUN_4200c604`), 8 PLUS sync
(`FUN_4200ae6c`), 9 `FUN_4200b2d8`.

## Flight record — `FUN_42010604`

512-byte record parsed from each feed row (sizes include the terminator):

| Field | Offset | Field | Offset |
| --- | --- | --- | --- |
| `icao` [8] | 0x00 | `cs` [12] | 0x08 |
| `reg` [—] | 0x14 | `alt_b`, `alt_g`, `vel`, `trk`, `vr` (double) | 0x28…0x48 |
| `gnd` | 0x50 | `sqk` [8] | 0x51 |
| `orig_icao`, `orig_iata`, `dest_icao`, `dest_iata` [8] | 0x59…0x71 | `orig`, `dest` [32] | 0x79, 0x99 |
| `al_code`, `al_paint`, `al_op` [8] | 0xb9…0xc9 | `al_name` [24] | 0xd1 |
| `ac_type` | 0xe9 | `ac_name` [32] | 0xf9 |
| `ac_class` | 0x119 | `deltas` (alt_b…vr, dt) | 0x130… |

`trk_info` (tracked flights only): `sts` one character `C` (cancelled) or `D`
(delayed) at 0x1f0 with `sexp` (0x1f4); `prog` (0x168) and `dist` (0x170)
doubles; strings `conf` [16] 0x178, `dep` [24] 0x188, `arr` [24] 0x1a0, `over`
[32] 0x1b8, `tft` [24] 0x1d8; `gnd` 0x1f1; present flag 0x1f8.

## Text clean-up — `FUN_42008680`

Applied to every feed string except `reg`, `orig_iata`, `dest_iata` and `sqk`:
`–` `—` → `-`; `'` `‘` `’` → `'`; `“` `”` → `"`; `…` → `...`; NBSP → space;
`°` unchanged. (The draw routine itself then maps the remaining characters,
see CLOCK.md "Stock text mapping".)

## Template tokens — `FUN_42008c18`

Each element's template is expanded token by token; missing values are `N/A`
unless noted.

| Token | Rule |
| --- | --- |
| `{al_name}` | `title_text` `registration` → `{reg}`; `callsign` → `{cs}`; otherwise first non-empty of al_name, cs, al_op, al_paint, al_code (`FUN_42008854`) |
| `{cs}` | callsign (`FUN_4200872c`) |
| `{reg}` | registration, raw; empty → `{cs}` (`FUN_4200879c`) |
| `{ac_name}` | aircraft name |
| `{orig}` | dest in `anchor_airports` → `Arriving from`; orig anchored → `Departing to`; else origin name; empty → `Unknown origin` on mini_v1 DEFAULT/NO_LOGO, else `N/A` (`FUN_4200898c`) |
| `{dest}` | dest anchored → origin name (or the unknown-origin rule); else destination name; empty → `Unknown destination` / `N/A` (`FUN_42008aa0`) |
| `{orig_iata}`, `{dest_iata}`, `{sqk}` | raw |
| `{alt_b}` | float altitude (m) → unit, number style below (`FUN_42007c44`) |
| `{vel}` | float speed (m/s) (`FUN_42007d9c`) |
| `{trk}` | `lroundf`, normalised 0–359; `cardinal` → 16-point name `((deg+11)/22)&15`; else `%ddeg` |
| `{vr}` | float vertical rate, magnitude in unit, `+` above 0.1 m/s, `-` below −0.1 (`FUN_42007f40`) |
| `{trk_prog}` | `%.0f%%` |
| `{trk_dep}`, `{trk_arr}`, `{trk_over}`, `{trk_conf}` | tracked strings (cleaned) |
| `{trk_tft}` | tracked string; missing → empty |
| `{trk_dist}` | float distance (m) (`FUN_420079e4`) |

Anchor matching (`FUN_420085c8`) compares ICAO and IATA codes upper-cased.

Units (`metric_display`), all from doubles truncated to float first. A value
is invalid (`N/A`) when NaN or infinite (`FUN_42007938`).

| Setting | Values → output |
| --- | --- |
| `alt_b_unit` | `feet` lround(d×3.28084) `ft`; `kilometers` lround(d/1000) `km`; `nautical_miles` lround(d/1852) `NM`; `meters` lroundf(f) `m` |
| `vel_unit` | `knots` ×1.943844 `kts`; `mps` lroundf `m/s`; `mph` ×2.236936 `mph`; `mach` `M%.2f` of f/343; `kmh` ×3.6 `kph` |
| `vr_unit` | `fps` ×3.28084 `ft/s`; `mps` `m/s`; `fpm` ×196.8504 `fpm`; `mph`; `kmh` `kph` (magnitude via lroundf of the float product) |
| `trk_unit` | `cardinal` or degrees |
| `dist_unit` | `miles` f×0.000621371f `mi`; `kilometers` f/1000 `km`; `nautical_miles` f/1852 `NM` (≥100 → `%ld`, else `%.1f`); `feet` lround(d×3.28084) `ft`; `meters` lroundf `m` (plain integers) |

Number style (`FUN_42007b4c`, alt/vel/vr): |n| ≤ 999 → `%d`; ≤ 9999 →
`%.1fk` of (float)(n/1000.0); else `n/1000` (truncated) + `k`. The unit follows
with no space: `35kft`, `5.3kft`, `480mph`.

## Element templates (stock defaults) — `FUN_42006f50`

| Layout | heading1 | heading2 | heading3 | footer1 | footer2 |
| --- | --- | --- | --- | --- | --- |
| mini DEFAULT | `{al_name}` | `{orig_iata}-{dest_iata}` | `{ac_name}` | `{orig}` | `{dest}` |
| mini NO_LOGO | `{al_name}` | `{orig_iata}-{dest_iata}, {ac_name}` | `{orig}` | `{dest}` | `Alt:{alt_b},Spd:{vel}` |
| mini TECHNICAL | `{al_name}` | `{orig_iata}-{dest_iata}` | `{ac_name}` | `Alt:{alt_b},Spd:{vel}` | `Trk:{trk},Vr:{vr}` |
| mini PLUS_SYNC_ACTIVE_MAIN | `{cs}` | `{orig_iata}-{dest_iata}` | `{ac_name}` | — | — |

Widescreen layouts add `tag1`–`tag4` (`Alt: {alt_b}`, `Spd: {vel}`, `Trk:
{trk}`, `Vr: {vr}`).

## Long text (owned)

Stock lets a line wider than its box run off the panel edge (below, "No
other truncation on the ordinary card"); the mini panel fits 21 characters
across and 14 beside the logo. The owned firmware, only for a line that does
not fit (`fw_flight_text`, `fw_track_text` except the tracked footers, which
keep stock's 18 + `...`):

1. **Shorter names**, then the line is expanded again:
   - airline (`{al_name}` from the airline name): a last `Airways`,
     `Airlines`, `Airline` or `Air Lines` dropped (`JetBlue Airways` →
     `JetBlue`, `Delta Air Lines` → `Delta`); never down to a lone `British`;
   - airports (`{orig}`, `{dest}`): `International` → `Intl`, `Regional` →
     `Rgnl`, `Municipal` → `Muni`, `Metropolitan` → `Metro`, `Executive` →
     `Exec`, `Memorial` → `Mem`, `County` → `Co`, `Airport`/`Airfield`
     dropped; still too wide, a last `Intl`/`Rgnl`/`Muni` dropped
     (`Dallas Fort Worth International` → `Dallas Fort Worth`). The owner's
     airport aliases are used as they are.
2. **Scrolling** (`components/fw_screens`): what still does not fit is drawn
   clipped to its box; after the card is drawn it stays still 1 s, moves left
   until its end shows, stays 1 s, and starts again. Speed is the owner's
   `owned_config.scroll_speed` (Display tab): slow 12.5 px/s, normal 25 px/s
   (a pixel per 40 ms panel pass, the default), fast 50 px/s. Only that line
   is redrawn, in opaque cells, so nothing flickers. Every full screen forgets
   the lines.
3. **The card waits** for its longest line to scroll through once and rest at
   its end (`fw_screen_scroll_pass_ms`): a rotation slot or a tracked page
   stays that long when it is longer than its usual time
   (`FwRotation.extra_ms`, cleared on each advance).

Tests: `tests/test_flight.c` (names), `tests/test_scroll.c` (timing, clipping,
redraws only inside the box).

## Drawing an element — `FUN_420097a4` (ordinary), `FUN_4200bc94` (tracked)

- Text is white (`0xFFFF`) whatever the layout colour; opaque 6×8 cells.
- Width = code points × 6 (no gap removed); height 8.
- x = element x, or x + w − width for `tag` style; y = y + (h − 8) / 2.
- heading1: with `title_text` `registration`, if the text is the registration
  and wider than the element, the callsign is used instead (`FUN_420087bc`).
- widescreen_v1 only: heading1/footer1 longer than 32 bytes → first 29 bytes +
  `...`. **No other truncation on the ordinary card**: long text runs to the
  panel edge.
- Only elements whose text changed are redrawn; the old extent (or, for a
  first draw, x−5…x+w+5 clamped to the panel) is cleared first.

## Tracked flight — `FUN_4200c290`, `FUN_4200bc94`, `FUN_4200b9f4`

State, from `trk_info`:

- **landed**: `prog` ≥ 100.
- **on ground before departure**: `prog` == 0 and `gnd`.
- **took**: landed and `tft` present.

Element templates (the layout's logo element still shows the airline logo):

| Element | Rule |
| --- | --- |
| heading1 | `{reg}` when `title_text` is `registration`, else `{cs}` |
| heading2 | `{orig_iata}-{dest_iata}`; on ground: `{ac_name}` |
| heading3 | `{ac_name}`; on ground: `{vel}` (mini_v1) |
| footer1 / footer2 | on ground: `On ground` / `Preparing to takeoff`; landed: `Took {trk_tft}` (only if took) / `Flight landed` **in green (0x07E0)**; otherwise by page below |

Pages (`FUN_4200e5f4`), shown in turn:

| Page | footer1 | footer2 | Included when |
| --- | --- | --- | --- |
| 0 | `Departed {trk_dep}` | `Arriving in {trk_arr}` | always |
| 1 | `Flying over` | `{trk_over}` | not in PLUS sync mode, or `over` present |
| 2 | `Alt:{alt_b},Spd:{vel}` | `Trk:{trk},Vr:{vr}` | metrics enabled and any of alt/vel/trk/vr valid |
| 3 | `{trk_dist}` | `Distance flown` | `dist` valid |
| 4 | sync message | | PLUS sync with a message |

`Arriving in now` is shown as `Arriving now`. On mini_v1, a footer over 21
code points becomes its first 18 plus `...`. Page time is 5 s; with more
than one tracked flight and more than two pages it is max(1 s, 10 s / pages).
A `{ac_name}` element shows `CANCELLED` (`sts` C) or `DELAYED` (D) while
0 < `sexp` − now ≤ 120 s.

Progress bar (`FUN_4200b92c`): a 1-px line at y = H − 2 from x = 2, W − 4
long, colour 0x4208; a green (0x07E0) part ⌊clamp(prog, 0, 100) / 100 ×
(W − 4)⌋ long when `prog` is known.

Open: the device-side flag that enables page 2 (offset 0x30a of the display
object) has no direct writer in the decompile; the owner's tracked-flight
`show_metrics` setting is the likely source.

## Several flights — `FUN_4200f3e4`, `FUN_4200e114`, `FUN_4200f048`

- One feed response holds at most 5 flight records (5 × 512 bytes, count at
  0xa00) plus a count of further flights (0xa2c).
- Rotation time per slot (`FUN_42005fb4`, `FUN_4200ed6c`):
  `flight_display_speed` `slow` 5 s, `fast` 2 s, anything else 3 s (floor
  0.5 s). A single flight is shown without rotating.
- `max_flights_behavior` (`FUN_420189b4`): `off`; `text` → with further
  flights, four flights plus a "too many" slot; `indicator_led` → all five plus
  an orange 2×2 mark at the top-right corner. Absent or invalid →
  `hide_max_flights_screen` decides (absent too → `text`).
- "Too many" screen (`FUN_4200959c`): cleared, frame pass, orange (0xFC00)
  `Too many flights,` (3,3), `reduce area or` (3,15), `add filters` (3,27),
  `Seeing %d flights` (3,52) with **5 + the further-flight count**.
- A new response keeps the aircraft on screen if it is still listed (ICAO
  address, else callsign) without restarting its time; otherwise the
  rotation restarts at the first flight. While on the "too many" slot it
  stays there.
- An empty response shows the loading template with `No flights...`; before
  any response, the loading template with no text (`CLOCK` shows the clock,
  `ELLIPSIS` `...`, `BLANK` nothing, otherwise the text centred at y = 28,
  `FUN_4201aaa4`).
- Frame pass after each screen (`FUN_4201a998`, `FUN_4201a93c`): with
  `border_enable` a 1-px border, grey at `border_brightness_percent`
  (r5 = pct×31/100, g6 = pct×63/100); the 2×2 corner block at (126,0) orange
  for the indicator, else border grey with (126,1) black, or black.

## Owned implementation

- `components/fw_flight`: the record, units, clean-up, template engine,
  element templates and tracked rules above, in portable C.
- `components/fw_screens`: `fw_screen_flight` (ordinary card) and
  `fw_screen_tracked` (tracked card with the progress bar) draw through them;
  text is opaque white (green for "Flight landed"), placed as stock does.
- `tests/test_flight.c` (144 checks): every unit and number style, names,
  anchors, clean-up, replacement order, element templates, heading1 fit,
  widescreen shortening, tracked states, pages, page timing, footers,
  cancelled/delayed window, "Arriving now", mini footer shortening, colours,
  progress bar and text placement.
- `tools/preview_screens.py` renders `tracked_departed`, `tracked_over`,
  `tracked_metrics`, `tracked_distance`, `tracked_landed`,
  `tracked_on_ground` and `flight_technical` with the owner's settings.

`components/fw_flight/fw_rotation.c` holds the several-flights rotation;
`fw_screen_too_many`, `fw_screen_message` and `fw_screen_frame` draw the rest
(tests: 244 flight checks in all).

Not yet built: parsing live feed rows into `FwFlight`, the PLUS sync and
upcoming screens, and stock's per-element incremental redraw (the owned
screens redraw whole frames, which ends in the same picture).

## On the board — verified 2026-10-19

Board run of `owned-skeleton-v1-8099b65a058f` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged). **The owner photographed the tracked card: callsign,
route and aircraft beside the AA logo, "Departed 35m ago" / "Arriving in 1h
10m", and the progress bar 42 % green over the grey line.**

| Measurement | Value |
| --- | --- |
| Tracked states logged | departed 4.8 s, over 10.1 s, metrics 15.1 s, distance 20.3 s, landed 25.1 s, on ground 30.1 s — the stock 5 s page time |
| Footer1 text logged | `Departed 35m ago`, `Flying over`, `Alt:6NM,Spd:518mph`, `300mi`, `Took 1h 52m`, `On ground` — as on the desktop |
| Panel task stack | 2,576 bytes free of 6 KB (tracked card is the deepest screen so far) |
| Route requests alongside | 3/3 verified, nothing retained; free heap after each 146,812 / 146,940 / 146,672 (no trend) |
| Lowest free heap | 116,380 (logo decoder overlapping Wi-Fi start-up, as before) |

## Several flights on the board — 2026-10-19

Board run of `owned-skeleton-v1-c52500f94f26` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged).

| Measurement | Value |
| --- | --- |
| Rotation logged | AAL1234 0.8 s, DAL402 5.9 s, UAL1871 11.0 s, SWA2290 16.0 s, "too many" (Seeing 12) 20.9 s, AAL again 26.0 s — the stock 5 s slots; JetBlue held back by the "too many" slot |
| Logos | AAL, DAL, UAL, SWA resolved and decoded to 32×32, rc 0; 70–137 ms each after the first (542 ms with the one-time mount) |
| Panel task stack | 2,752 bytes free at the deepest |
| Lowest free heap | **95,016** — a 44 KiB logo decode overlapped a TLS round at 16 s |
| Route requests alongside | 3/3 verified, nothing retained; free heap after each 142,664 / 142,648 / 142,452 |

The 95 KB low point confirms the earlier note: in the real firmware, logo
decodes must not run during TLS work (serialise them, and cache each
airline's 2 KB result).
