# Analytics — bounded, on the device

The stock firmware has no analytics (Plus lists them as "coming soon"), so
this is an owned design, following OWNED-FIRMWARE-PLAN.md step 4: aggregate on
the board, bounded, with coverage gaps made explicit; charts are drawn in the
browser.

## What is kept (`components/fw_analytics`)

- **Sightings, not polls.** Every flight in a feed response is passed in
  before the display picks its five. A flight (by ICAO hex, else callsign,
  else registration) seen again within 30 min is the same sighting; unseen
  longer, it counts again. Deduplication is a fixed 512-entry set-associative
  table (64 sets × 8 ways, least recently seen replaced), so memory never
  grows; the most recent flights are always remembered.
- **Per local hour, 48 hours**, and **per local day, 31 days**: sightings and
  the minutes actually observed. A minute counts as observed when a feed poll
  succeeded in it, so sleep, outages and restarts show as gaps (missing hours,
  or low minute counts) rather than as quiet periods. Local time is the
  board's UTC offset, so days follow the owner's clock.
- **Top airlines per day**: space-saving top-12 (Metwally et al.); counts are
  upper bounds within the recorded `error`, exact for airlines that never
  dropped out — the frequent ones.
- **JSON export** for the browser: hours and days oldest first, with the
  day's airlines.
- **Checkpoint**: hours, days and totals in a versioned little-endian blob
  under 4 KiB with a CRC-32; a damaged or foreign blob is refused and the live
  state is kept. The deduplication table is not saved (after a restart a
  flight still overhead may count once more).

The whole state is about 8 KB of RAM.

## On the wall (`main/stats.c`)

- **Fed by both sources.** The vendor feed passes the flights each good poll
  lists (at most five; the rest arrive only as a count, so busy hours read
  low; tracked-flight answers are skipped). The community feed passes every
  aircraft that clears the owner's filters, in every area, not only the eight
  it keeps. Each good poll marks its minute as watched.
- **Local time** is the owned settings' zone with daylight saving (no zone =
  UTC), taken once per poll.
- **Airline**: the flight's airline code, else the first three letters of a
  callsign such as `DAL1234`; private and unknown aircraft count as
  sightings without an airline.
- **Memory only** for now (about 8 KB, allocated at start): a restart starts
  over. Thread-safe (the feed task writes, the dashboard reads).
- **Dashboard:** `GET /api/stats` (signed in) →
  `{"hour","day","offset","stats":<fw_an_json>}`; the **Stats** tab draws today's
  count and top airlines, the last 48 hours and the last 31 days as bar
  charts, unwatched slots shaded and partly watched ones faded.

## Not decided yet

- **Where checkpoints live.** Not the small settings namespace and not the
  vendor asset partition (plan constraints). Options: a dedicated NVS
  namespace with a few blobs (needs measuring free NVS pages) or a slice of
  free flash; saving every 15–30 minutes and before a planned restart, never
  per observation.
- **Longer history / export** after storage headroom is measured.

## Verification

`tests/test_analytics.c` (run by `tests/run_host_tests.py`): flight identity,
local hour/day boundaries in several zones, the revisit window, a 5 000-flight
flood (bounded, recent flights remembered), hour and day rings and stale
observations, minute coverage, airline ranking with ties and case folding,
space-saving with more airlines than slots, exact JSON, and the checkpoint
round trip, truncation and corruption at every 37th byte.
