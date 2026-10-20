# Plus Sync — recovered from the vendor firmware

Stock's "Plus Sync" (crew schedule legs on the display), recovered from vendor
app `0ab195d6…` (whole-program Ghidra decompile and the DROM geometry
tables). Tags: **[O]** observed, **[I]** inferred, **[U]** unknown.

## Owned implementation

- `components/fw_sync`: the leg record, derived strings, ISO time parser,
  fit and word wrap, ACTIVE_MAIN footers and message page, ACTIVE_MESSAGE
  context lines, progress, phase lengths, and the screens
  PLUS_SYNC_ACTIVE_MESSAGE, PLUS_SYNC_UPCOMING_NEXT and PLUS_SYNC_UPCOMING_LIST
  for mini-v1, drawn from the firmware's geometry tables (not
  `vendor-layouts.json`, which differs for the mini list and the strip-in
  flag). Desktop test `tests/test_sync.c` (134 checks); previews in
  `tools/preview_screens.py`.
- ACTIVE_MAIN, the phase rotation and a source for legs are now owned
  (see "Owned Sync"); the owned firmware writes `dep_rel`/`arr_rel` itself in
  the form stock's rules read: `in …` before, `… ago` after, `<n>d` for days
  (which hides the arrival on the next-leg screen). Not yet: the wide-panel
  tables.

## Owned Sync

The vendor's Plus Sync gets legs from its paid service. The owned firmware has
its own: legs loaded on the dashboard (**Sync** tab), kept on the wall and
worked out from its own clock into the same record and screens.

- **Loading legs:** by hand (airline, flight number, airports, date and local
  times; an arrival earlier than the departure is the next day) or from a
  calendar file (.ics) exported by a crew or calendar app. Each event's title,
  location and description are read for the flight number (`AA 1234`,
  `B6515`), the route (`CLT-PBI`, `JFK to SJU`), a deadhead (`DH`, `PAX`) or
  reserve (`RES`) marker, and `STATUS:CANCELLED`; times with a `TZID` are
  converted with the browser's own time zone data. Every flight is shown for
  ticking first; past events and ones without a flight number are left out.
- **On the wall** (`components/fw_sync/fw_schedule.c`, tested by
  `tests/test_schedule.c`): up to 20 legs, a message home (255 bytes) and
  stock's options (`show_upcoming`, `main_view`, `upcoming_view`) plus whether
  deadhead and reserve legs show (hidden by default, as the vendor describes).
  Each 15 s: the **active leg** is the one in the air, else one that landed
  under 30 min ago, else the next departing within 2 h (never a canceled one);
  **upcoming** are the next five departures (canceled ones show "Canceled");
  relative times are written the way stock's rules read them: `in 2h 05m`,
  `in 45m`, `in 3d`, `35m ago`.
- **Panel:** stock's phases and screens: the trip card (ACTIVE_MAIN, now
  drawn: page 0 with "Departed …" / "Arriving in …", page 4 the message) or
  the message screen, and the upcoming legs one at a time or as a list.
  Typed airline codes (`AA`) map to ICAO (`AAL`) for logos.
- **Storage:** one NVS blob, `fw_owned/sched` (667 bytes for 20 legs, CRC),
  written only when a changed schedule is saved: no routine flash writes.
- **API:** `GET`/`PUT /api/sync` (signed in), the whole schedule each time.
- **Live trip** (community source): while a leg is in its window (30 min
  before departure to 30 min after arrival) the wall looks its callsign up on
  adsb.fi once a minute (`AA 1234` → `AAL1234`) and the airports' coordinates
  once (adsbdb). The trip card then runs stock's tracked pages on the real
  aircraft (no "Flying over" in Sync): "Departed 35m ago / Arriving in 1h 05m"
  (time left at the current ground speed), altitude and speed, distance flown;
  progress along the great circle; "landed" once down within 10 km of the
  destination; then the message. Without a sighting it stays on the scheduled
  card. Logged as `sync_live`, counts only.
- **Live trip on the vendor feed:** in the same window the leg is asked for
  as one more tracked flight (`al`+`fn`, e.g. `AA1234`; `auto_remove` false,
  distance and metrics on; not added twice when the owner tracks it already;
  with 8 tracked it takes the last place), only in the request: the owned
  settings are not changed. The vendor's tracked answer then drives the same
  pages; the record is the one whose `cs` is `al`+`fn` (stock's rule) or its
  ICAO form, with other tracked flights beside it allowed (stock wants
  exactly one). `feed_configured` logs `sync_tracked`. Tracked flights are
  free on the vendor side (not a Plus feature; the owner, 2026-10-20).
- **Not yet:** calendar subscriptions and roster formats.

 (in the decompile or data), **[I]** inferred, or **[U]** unknown. The "rot" offsets are relative to the rotation object (display + 0x1e88).

## 1. Data

**Settings [O] `FUN_42003fa8`** (called from `FUN_42005fb4` on the stored-config key `plus`). The result is packed into config+0x148 (default 0x0100), then copied to rot+0x56e..0x571 (`FUN_42011134`).

| Key | Type / default | Meaning |
|---|---|---|
| `plus.sync.enabled` | bool, false | rot+0x56e |
| `plus.sync.display.show_upcoming` | bool, **true** (also true when `display` is missing) | rot+0x56f |
| `plus.sync.display.main_view` | `"active"` / `"message"`; anything else → active | rot+0x582 (1 = message) |
| `plus.sync.display.upcoming_view` | `"next"` / `"list"`; anything else → next | rot+0x583 (1 = list) |

**Transport [O].** There is no separate sync endpoint. Sync data comes in the normal flight response: a POST to `http://api.theflightwall.com/v2/flights` with an `x-api-key` header and msgpack content type (`FUN_42014fe0`). A JSON response is also accepted (`FUN_420117e4`). The poll cadence is the ordinary flight fetch (`ulTaskNotifyTake(…,10000)` in `FUN_4201563c`) **[I]**.

**Response keys [O]**: msgpack `FUN_4200d594`, JSON `FUN_420117e4`.

| msgpack | JSON | Stored (feed buffer) |
|---|---|---|
| `v` | — | must be ≤ 1, otherwise the response is rejected |
| `f` / `tf` | `flights` / `tracked_flights` | ≤ 5 × 512-byte records; count at 0xa00; 0xa30 = 1 for `tf` |
| `ch` | `config_hash` | str[40] at 0xa01; a mismatch triggers a reconfigure (`FUN_42015b34`) |
| `ov` | `overflow` | u16/int at 0xa2c |
| `sla` | `sync_leg_active` | one leg at 0xa34; flag 0xc75 = 1 |
| `slu` | `sync_legs_upcoming` | array of legs at 0xa94 + i·0x60, **max 5** (extra are skipped); count byte 0xc74 |
| `scm` | `sync_custom_message` | str at 0xc76, ≤ 255 bytes; a trailing partial UTF-8 sequence is cut (`FUN_4200b1b0`) |

The buffer is cleared at the start of each response (`FUN_4200b150`).

**Leg record, 0x60 bytes [O]** (`FUN_4200cd8c` msgpack, `FUN_4200fe10` JSON, init `FUN_4200a01c`). Every leg key is short, in both encodings.

| Key | Off | Size | Use |
|---|---|---|---|
| `lt` | 0x00 | str[12] | leg type: `operating` / `deadhead` |
| `ls` | 0x0c | str[12] | leg status: `canceled` (the only value tested) |
| `al` | 0x18 | str[8] | airline code (also the logo) |
| `fn` | 0x20 | str[8] | flight number |
| `orig`, `dest` | 0x28, 0x30 | str[8] | airport codes |
| `dep_rel`, `arr_rel` | 0x38, 0x48 | str[16] | **server-formatted relative times**, e.g. `in 2h 05m` or `35m ago` [I: examples]. The device computes no countdown text. |
| `dep_at`, `arr_at` | 0x58, 0x5c | ISO-8601 string → u32 epoch | used only for the progress bar |

ISO parser `FUN_42009db4`/`FUN_42009cfc` [O]:
- Format: `YYYY-MM-DD` then `T`, `t` or space, then `HH:MM[:SS]`, an optional `.frac` or `,frac` (ignored), then `Z`, `z`, `±HH[[:]MM]` or nothing (nothing means UTC).
- Second 60 becomes 59.
- Failure gives 0.

**Derived strings [O]:**
- *flight* = `al`+`fn`, only when both are non-empty, else empty (`FUN_4200a0e8`/`FUN_4200a070`).
- *route* = (`orig` or `---`) + `-` + (`dest` or `---`) (`FUN_4200a17c`).
- *status* (`FUN_4200a0b0`) is the first that applies:
  - `ls`==`canceled` → `Canceled`
  - `lt`==`operating` → `Operating`
  - `lt`==`deadhead` → `Deadhead`
  - otherwise empty
- *strip-in*: a leading `in ` (3 bytes, case-sensitive) is removed (`FUN_4200a274`).

## 2. States and timing [O unless noted]

**New response (`FUN_4200f3e4`):**
- **Active leg:** when sync is enabled and `sla` is present, `FUN_4200e764` sets it as the active leg (rot+0x584) and the message (rot+0x58c, if non-empty).
  - **Live tracked flight** (rot+0x588) is set only when all of these hold: the response is `tf`, it has exactly **1** flight, and that flight's `cs` strcasecmp-equals `al`+`fn`.
  - It also builds a **synthetic tracked record** at rot+0x5a0: `cs` = flight, `al_code` = `al`, `orig_iata`/`dest_iata` = `orig`/`dest` or `---`, every double NaN, no `trk_info`.
- **Upcoming alternation** (rot+0x570) is on when sync is enabled, `show_upcoming` is on and the upcoming count > 0.
  - On first enable the phase is main (0) if there are flights; with no flights it is main only if there is an active leg, else upcoming.
  - If there are no flights and no active leg, the phase is forced to upcoming.
- **"No flights..."**: shown when the response has no flights, no active leg, and either no upcoming legs or `show_upcoming` off.

**Per display pass (`FUN_42015fb8`):**
- Phase 0, "main", goes through `FUN_4200f048`. If there is an active leg it *always* shows the ACTIVE screen (`FUN_4200ef34`), never the ordinary flights; otherwise the ordinary or tracked rotation.
- Phase 1, "upcoming", goes through `FUN_4200f2b8`: list (mode 9) or next (mode 7). With nothing to show it falls back to phase 0.
- **Phase length** = mult × *slot*. *Slot* is `flight_display_speed`: slow 5000, fast 2000, else 3000 ms (rot+0x57c via `FUN_4200ed6c`). The mult is:
  - **phase 0, no active leg:** number of flights (min 1)
  - **phase 0, main_view message:** number of context lines (`FUN_4200a5b4`)
  - **phase 0, active, no live flight:** 1, or 2 when there is a message
  - **phase 0, active, live flight:** tracked page count (rot+0x2f0)
  - **phase 1:** upcoming count (min 1)
- **Active-screen sub-rotation** (`FUN_4200edb8`): the index rot+0x590 advances every **>4999 ms**, modulo n, independent of the slot. It sets:
  - which context line ACTIVE_MESSAGE shows (n = context count);
  - which ACTIVE_MAIN page shows without a live flight (n = 1, or 2 with a message): index 0 → page 0, index 1 → **page 4** (the message).
- **With a live flight** ACTIVE_MAIN uses the ordinary tracked page rotation (pages 0–4, 5 s each; see FLIGHT-SCREENS.md). The ident phase is the tracked page index.
- **UPCOMING_NEXT:** shows each leg for max(500, slot) ms, cycling i = (i+1) % count. The index and timer survive phase changes, so on re-entry it usually advances at once **[I]**.
- **UPCOMING_LIST:** drawn once per entry into the phase (static).

## 3. Screens

Common to all screens [O]:
- Text width = code points × 6 × scale; height = 8 × scale (per CLOCK.md) **[I]**.
- Drawn with `FUN_4201a7bc(x, y, text, colour, bg 0, scale)`.
- Full clear (`FUN_4201a7dc`), then the frame pass (`FUN_4201a998`), on every redraw.
- **fit(s, n)** (`FUN_4200a8b0`): if s has more than n code points → its first n−1 code points, trimmed **[I: `FUN_4203de94` = trim]**, + `.`; n = 0 → empty.

Geometry is in DROM tables [O]:
- mini: msg `0x3c0f203c`, next `0x3c0f2760`, list `0x3c0f1f78`
- wide: `0x3c0f1fdc`, `0x3c0f26d4`, `0x3c0f1f14`

It matches vendor-layouts.json except for the differences noted below.

### ACTIVE_MAIN (mode 1, layout `PLUS_SYNC_ACTIVE_MAIN`; `FUN_4201033c`, `FUN_4200b634`, `FUN_4200bc94`, `FUN_4200c290`) [O]

This is the tracked card, drawn on the live record or else the synthetic one. Templates come from FLIGHT-SCREENS.md:
- heading2 `{orig_iata}-{dest_iata}`
- heading3 `{ac_name}`: empty on the synthetic record. It most likely shows `N/A` **[I]**. The CANCELLED/DELAYED override is off in sync.
- logo: from `al`

What sync changes:
- **heading1** = ident (no templates): flight, or status when the phase is odd and both are non-empty. Only one non-empty → that one. With `title_text`=`registration` and a record holding a reg or cs → `{reg}` instead of the flight.
- **Page 0 footers** (`FUN_4200a298`), first case that applies:
  - live flight with `trk_dep` → `Departed `+trk_dep / `Arriving in `+trk_arr (if non-empty);
  - canceled → `Flight canceled` / `Scheduled `+dep_rel (if dep_rel non-empty);
  - dep_rel empty → both blank;
  - dep_rel ends in `ago` → `Departed `+dep_rel / `Arriving in `+strip-in(arr_rel) (if non-empty);
  - else → `Departs `+dep_rel / `Arrives `+arr_rel (if non-empty).

  `Arriving in now` becomes `Arriving now`.
- **Page 4** = the message word-wrapped into footer1/footer2 with n = footer w/6 (mini 20/20), then fit(.).
- Pages 1–3 are as tracked. **[I]** The decompile's flow is tangled here.
- **Progress bar:**
  - live `prog` when valid, else from times;
  - from times: canceled, no times, or `dep_at`≥`arr_at` → NaN; now ≤ dep → 0; now ≥ arr → 100; else (now−dep)/(arr−dep)·100;
  - same bar as tracked (1 px at y = H−2, x = 2, W−4 long, grey 0x4208, green 0x07E0);
  - NaN → a **black** 1-px line instead.

### ACTIVE_MESSAGE (mode 8, `FUN_4200ae6c`) [O]

| Element | mini x,y,w,h | scale | colour | Content |
|---|---|---|---|---|
| msg1 / msg2 | 2,2,124,18 / 2,20,124,18 | 2 | FFFF | message (blank if none) |
| ident | 2,39,124,10 (wide y 38) | 1 | FFFF | flight/status, by bit 0 of the phase (as above); fit(w/6) |
| context | 2,52,124,10 (wide y 51) | 1 | FFFF | lines[phase % n]; fit(w/6) |

**Message wrap (`FUN_4200aab8` + `FUN_4200a958`):**
- **Words:** split on spaces (0x20 only; leading spaces skipped). Greedy: a word goes to the next line when the current line is non-empty and len+1+word > max and a next line exists. The last line takes everything left.
- **Scale:** try 2 (mini max 10 chars per line, wide 21), then 1 (mini 20, wide 42). Pick the first scale at which both lines fit. If none fits, use scale 1 and fit(.) each line.
- **Scale 2:** each line is centred in its own box, x + (w−tw)/2 and y + (h−16)/2, only when the slack is > 1. Mini rows are y = 3 and 21.
- **Scale 1:** the n non-empty lines are stacked with a 2 px gap, centred in the y span from msg1.y to msg2.y+msg2.h. Mini: two lines at y 11/21, one line at y 16. x is centred in msg1.w.
- **Ident/context placement:** x centred, y + (h−8)/2 (mini 40 and 53).

**Context lines (`FUN_4200a648`), in order:**
1. route
2. if not canceled: `Arriving in ` + (live `trk_arr`, else strip-in(arr_rel)), when non-empty
3. live `trk_over` → `Over `+over
4. live distance valid → `{trk_dist}` + ` flown`

**Progress bar:** as on ACTIVE_MAIN, but a NaN progress draws **nothing**.

### UPCOMING_NEXT (mode 7, `FUN_4200c604`, `FUN_4200c51c`) [O]

No vertical centring; y is the element y. Each text is `snprintf("%s%s", prefix, text)` into 40 bytes, with no fit.

| Element | mini x,y,w | scale | colour | Text |
|---|---|---|---|---|
| header | 4,4,120 | 1 | 07FF | `UPCOMING` if count < 2, else `UPCOMING %u/%u` (i+1, count) |
| flight | 4,16,120 | 2 | FFFF | flight |
| route | 4,34,120 | 2 | FFFF | route |
| dep | 4,54,60 | 1 | 07E0 | `Dep `+dep_rel. Mini applies strip-in; wide does **not** (table flag differs from the JSON). Skipped if empty. |
| arr | 68,54,56 (wide 130,54,122) | 1 | 07FF | `Arr `+arr_rel (strip-in on mini), right-aligned: x += w−tw when tw < w. **Skipped if empty or if it contains a digit followed by `d`** (e.g. `1d 2h`). |

### UPCOMING_LIST (mode 9, `FUN_4200b2d8`, `FUN_4200b218`) [O]

**Header** (2,2) cyan, scale 1, fit(w/6): `UPCOMING` when count = 1, else `UPCOMING - NEXT %u` (count capped at 5).

**Rows** i = 0…count−1: y = 10 + 11i + 1 (11, 22, 33, 44, 55). Each cell is fit(w/6) at scale 1. A column with w = 0 is skipped (mini has no flight or arr column). Right-aligned cells: x = max(x, x+w−tw).

| Column | mini x,w | wide x,w | colour | Text |
|---|---|---|---|---|
| index | 2,12 | 2,12 | 07FF | `1`…`5` |
| flight | — | 16,48 | FFFF | flight |
| route | 14,44 | 66,44 | FFFF | route (7 chars on mini) |
| dep | 58,54 | 112,66 | 07E0 | dep_rel raw (no strip; 9 chars on mini) |
| arr | — | 180,60 right | 07FF | arr_rel raw |
| flag | 112,14 right | 242,12 right | 8410 | `X` canceled, `DH` deadhead, else blank |

## 4. Custom message ("messages home") [O]

- **Source:** server field `scm` / `sync_custom_message`. Plain UTF-8 text, ≤ 255 bytes, no markup.
- **When it shows:** only alongside an active leg (`sla`). The device writes no other text of its own.
- **ACTIVE_MESSAGE:** the 2-line block above.
- **ACTIVE_MAIN:** page 4. Without a live flight it alternates with page 0 every 5 s and the main phase is 2 slots long. With a live flight it is the last tracked page.
- **Empty message:** no page 4, and ACTIVE_MESSAGE's top area stays blank.
- **Text rules:** every text goes through the stock draw mapping (CLOCK.md). Feed clean-up (`FUN_42008680`) is **not** applied to leg or message strings **[I]**: no call was seen in the sync parsers.

## 5. No new `{tokens}` [O]

The sync screens build their text in code. They add no `{tokens}` to the template engine, and only reuse `{trk_dist}` for the "flown" line.

## Unknown or unverified

- **[U]** The exact text of server `dep_rel`/`arr_rel` values. The device only recognises an `in ` prefix, an `ago` suffix and a `<digit>d` pattern.
- **[U]** `lt` values beyond `operating`/`deadhead`.
- **[I]** Truncation of msgpack strings at size−1 (`FUN_4200c850`), `FUN_4203de94` = trim, and the font metrics.
- **[I]** On ACTIVE_MAIN, `{ac_name}` on the synthetic record shows `N/A`.
