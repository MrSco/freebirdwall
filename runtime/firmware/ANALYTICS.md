# Analytics — bounded, on the device

Owned on-device implementation matching Plus Analytics (5.0): aggregate on the
board in bounded RAM (~15.5 KiB total), stream JSON directly with zero heap,
and draw the heat map and rankings in the browser.

## What is kept (`components/fw_analytics`)

- **Sightings, not polls.** Every flight in a feed response is passed in before
  the display picks its five. A flight (by ICAO hex, else callsign, else
  registration) seen again within 30 min is the same sighting; unseen longer, it
  counts again. Deduplication is a fixed 512-entry set-associative table (64
  sets × 8 ways, least recently seen replaced), so memory never grows.
- **Per local hour (48 hours) and per local day (31 days):** sightings, minutes
  actually observed (so sleep, outages and restarts show as gaps rather than
  quiet periods), and `routes_known` (how many sightings had an origin or
  destination airport). Local time follows the board's UTC offset + DST.
- **Top airlines, aircraft types, departure & arrival airports per day:**
  space-saving top-12 airlines and top-6 aircraft types, origin airports, and
  destination airports per day across all 31 days. The dashboard aggregates
  **Today**, **This week (7 d)**, and **Last 30 days** right in the browser.
  Community routes that arrive asynchronously from `adsbdb` are backfilled once
  per sighting (`fw_an_route`).
- **Rare aircraft badges:** a flash-resident curated rarity table (~165 ICAO
  type codes in `One of a kind`, `Very rare`, and `Notable` tiers; 0 bytes of
  RAM) records the 16 most recent rare sightings with type, callsign,
  registration, airline, route and timestamp.
- **Flight-path heat map:** a 40×40 spatial density grid (~0.5–0.8 km² per cell
  for a typical watch area) with auto-scaling and a 32-entry active-track table
  that Bresenham-interpolates continuous corridor lines between consecutive
  position fixes of each aircraft.
- **Streaming JSON export** (`fw_an_json_stream`): streams directly into HTTP
  chunks with zero heap allocation and no `fw_big` contention.
- **Checkpoint v2** (`fw_an_save` / `fw_an_load`): hours, days (airlines, types,
  airports, routes), rare sightings, sparse/dense heat map, and totals in a
  versioned little-endian blob (up to 12 KiB) with CRC-32; damaged or foreign
  blobs are refused and live state is kept.

## On the wall (`main/stats.c`, `main/explore.cpp`)

- **Fed by both sources.** The community feed passes every aircraft that clears
  the owner's filters (with live lat/lon for the heat map and async route
  backfill from `adsbdb`). The vendor feed passes each listed flight (at most
  five per poll), including optional `lat`/`lon` if present, plus any `adsb.fi`
  positions fetched for the progress bar (`fix_step`).
- **Explore anywhere in the world (`POST/GET /api/explore`):** on-demand
  rate-limited live airspace snapshots anywhere on Earth via `opendata.adsb.fi`
  through `feed_task` (byte-by-byte streaming scanner, 0 bytes idle RAM). The
  dashboard can search any city/airport and optionally auto-refresh every 15 s
  to build live flight-path corridors in the browser.
- **Persistence & backup (zero flash wear):**
  - Stats stay in RAM on the board so the 20 KiB NVS partition is never worn or
    overflowed.
  - **Browser download/restore:** `GET /api/stats/checkpoint` and
    `POST /api/stats/checkpoint` export/import the `.fwan` binary checkpoint.
  - **Optional LAN/NAS backup server:** configure `owned_config.stats_url` (e.g.
    `http://192.168.1.50:8090`) and run `py desk/stats_server.py` on a NAS, Pi,
    or home server. The wall restores its checkpoint on boot (`GET /checkpoint`)
    and backs up every 15 minutes when new sightings arrive or on demand
    (`POST /api/stats/sync` → `PUT /checkpoint`).

## Verification

- `tests/test_analytics.c` (via `tests/run_host_tests.py`): flight identity,
  local timezones, revisit window, 5 000-flight flood, coverage gaps, airline /
  aircraft type / airport top-K rankings, async route backfill, rarity lookup,
  heatmap Bresenham tracks + relocation reset, JSON export, and v2 checkpoint
  round-trip + corruption checks.
- `desk/test_stats_server.py`: LAN/NAS checkpoint server round-trip and header
  validation.
