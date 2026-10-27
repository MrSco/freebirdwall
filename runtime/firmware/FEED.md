# Vendor flight feed — recovered from the vendor firmware

How the stock firmware gets its flights (vendor app `0ab195d6…`, whole-program
Ghidra decompile). Tags: **[O]** observed, **[I]** inferred, **[U]** unknown.
Secrets are named by NVS key and length only.

## Owned implementation

- `components/fw_feed` (portable C): the MessagePack response parser with
  stock's exact type rules (`fw_feed_parse`), the request body from the stored
  configuration (`fw_feed_body`), the display settings (`fw_feed_display`) and
  a bounded HTTP response reader (`fw_http_*`). Desktop test
  `tests/test_feed.py`: 123 checks, fixtures encoded with the independent
  msgpack library.
- `main/feed.c`: reads `api/key` and `usercfg/json` (read-only), then polls
  `POST http://api.theflightwall.com/v2/flights` every 10 s with stock's
  headers and body; only a 200 MessagePack body up to 5 KiB counts; the last
  good list stays on any failure. Differences from stock: one connection per
  poll (stock keeps one alive) and a backoff after repeated failures
  (20/40/60 s). No status reports to the vendor. A changed `config_hash`
  fetches the app's configuration and merges it setting by setting (SETTINGS.md
  "Changes made in the app"; stock adopts it whole).
- `main/display.cpp` live mode: flights through the stock rotation, tracked
  flights page by page, the stored `loading_template` when there are none,
  "Configure settings / in the app to / display flights..." without a stored
  configuration; units, title, layout, speed, max-flights behaviour, border and
  clock from `display_config`. Without an API key the demo screens run.
- Airport aliases (an owned feature; the vendor firmware has none): the
  table our owner patch keeps in NVS `fw_owner/aliases` (format and rules from
  `shared/owner_alias.c`, used as is) is read at start, read-only,
  and applied when `{orig}`, `{dest}`, `{orig_iata}` and `{dest_iata}` are
  substituted (`FwFlightSettings.alias`, `main/aliases.c`). Editing them comes
  with the authenticated settings.
- The community feed (owned; below) and the source switch.
- The clock's extra time zones (`clock.additional_timezones`, CLOCK.md) and the
  "no zone" default for a missing main zone.
- Not yet: Plus Sync phases on the panel, JSON responses,
  "Scan to play" inside the live rotation.

## Tracked flights on the community source (owned)

The vendor finds tracked flights on its server; on the community source the
wall finds them itself (`main/community.cpp`, "tracked flights"):

- **Callsign:** the flight number becomes the callsign it flies under
  (`fw_sched_callsign`: `AA1234` → `AAL1234`, `DL0012` → `DAL12`; a
  three-letter prefix is kept, an unknown airline is kept as typed).
- **Position:** adsb.fi `/api/v2/callsign/<cs>`, **once a minute** for a
  flight in the air; the airports' coordinates and names once (adsbdb).
  Nothing is asked when nothing is tracked.
- **Times,** chosen on the dashboard (`owned_config.tracking_source`):
  - `community` (default): flights not yet in the air are looked for in turn,
    one lookup a minute for all of them together. Take-off is when the wall saw
    it leave the ground, or an estimate (distance flown / speed); arrival is
    distance left / speed.
  - `aeroapi`: the owner's own FlightAware AeroAPI key
    (`fw_owned/aero_key`, written only when saved or removed on the
    dashboard; never logged or sent back, the page sees its last four
    characters). `GET https://aeroapi.flightaware.com/aeroapi/flights/<cs>
    ?ident_type=designator&start=…&end=…&max_pages=1` (header `x-apikey`, SSL.com
    RSA root pinned; `main/aero_root.h`) gives the times and status, read
    through a filter (`components/fw_community/fw_aero.cpp`): the flight in
    the air, else one landed in the last 30 min, else the next to leave (not
    cancelled; up to 3 h late). Asked about every 12 h with nothing near, 2 h
    before departure, every 30 min before take-off and every hour in the air;
    adsb.fi is asked only from 30 min before take-off. When adsb.fi has missed
    a flight FlightAware says is in the air three times, FlightAware's
    `/flights/<id>/position` every 5 min. A 401/403 stops all requests until
    the key changes. Cost on the Personal tier ($5 free a month): $0.005 per
    times request, $0.01 per position; the dashboard counts them since start.
    On the desk the handshake fits the 48 KiB arena (peak 39,160 bytes).
- **Shown:** with any tracked flight in sight the panel shows those (up to
  five) as stock shows a tracked answer; otherwise the nearby flights.
  `show_metrics` and `show_distance_travelled` are honoured; `auto_remove`
  holds a landed flight back until restart (the settings are not written).
  The "Flying over" page is skipped (no place names on this source).
- Logged as `tracked_lookup` (counts and HTTP status only).

## Progress bar positions on the FlightWall feed (owned)

With `owned_config.progress_bar` on, the vendor source also asks adsb.fi for
the positions of the flights on screen, by ICAO address, one request per poll
at most (FLIGHT-SCREENS.md "Progress bar"). Off, nothing is asked.

## Community feed (owned)

A second source the vendor firmware does not have: aircraft positions from
[adsb.fi](https://adsb.fi) and routes from [adsbdb](https://www.adsbdb.com),
chosen on the dashboard (**Flights › Flight data**, stored as
`owned_config.flight_source` = `community`; the vendor feed is the default).
It needs no vendor account or key.

- **Positions:** `GET https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>`
  for each area in turn (radius km → nautical miles), at least 5 s apart for
  all areas together (adsb.fi allows one request a second), with backoff and
  `Retry-After` on failures. The answer is read as it arrives, one aircraft
  (at most 4 KiB of JSON) at a time; responses up to 128 KiB.
- **Routes:** `GET https://api.adsbdb.com/v0/callsign/<callsign>` for flights
  still without one, two per step at most, 1.5 s apart; a callsign is asked
  again after a minute at the earliest. A route must name the same callsign
  and both airports. Routes are kept only in memory with their flight, never
  stored (adsbdb's route data may not be copied into other databases).
- **Filters on the wall:** the areas (great-circle distance, altitude limits
  in metres), speed limits, the airline list (from the callsign's logo code),
  aircraft kind (airliner = a known airline logo, helicopter = ADS-B category
  A7, everything else "other"; light aircraft and business jets cannot be told
  apart from ADS-B alone) and "flights with no known route". Tracked flights and
  drawn (geo) areas need the vendor feed and are left out, counted in
  `feed_configured` (`skipped_tracked`, `skipped_geo`); the dashboard says so.
- **Shown:** the eight nearest kept flights (normalised by each area's radius),
  the first five on screen, the rest as the "more flights" count. The records
  become the same `FwFeedFrame` as the vendor feed's (`fw_community_frame`):
  SI units, airline code for the logo, adsbdb airport names shortened
  ("London Heathrow Airport" → "London Heathrow", "… International Airport" →
  "… Intl") and aircraft names in title case, so the screens, logos and
  airport names work unchanged. A new list is published only when it differs.
- **TLS:** the source-built engine (`fw_tls`, [net/README.md](../net/README.md)),
  certificate and host name always verified against two pinned roots, GTS
  Root R4 (adsb.fi) and ISRG Root X2 (adsbdb), each accepted for both hosts.
  Requests wait until SNTP has set the clock.
- **Memory:** the TLS arena is a 48 KiB block reserved at link time and shared
  with the logo decoder (`main/big_buffer.c`); a logo due while a request holds
  it is drawn a moment later. The source's own state (about 22 KiB: settings,
  three 8-record lists, one JSON document, one row buffer, the roots) is
  allocated the first time it is chosen. AES tables are in flash.
- **Switching** clears the other source's list at once, so one source's
  flights are never shown as the other's.
- Logs (`community_poll`, `community_route`) carry counts, statuses and
  timings only, never positions, callsigns or the owner's areas.
- Code: shared reader, filters, cache and request policy in
  `runtime/src` and `esp32/stream_adsb.cpp` (ctest: 6 suites);
  `components/fw_community` (`tests/test_community.py`); the worker
  `main/community.cpp`; the switch in `main/feed.c`.

### On the board — 2026-10-19

Test build `68f91bbe85ff` (pending, rolled back after): started on the vendor
feed, polls 200 with logos decoded in the shared block (lowest free heap
65 KB). The owner switched to Community on the dashboard; the feed was
reconfigured at once, the state went live, and the panel showed community
flights with logos and origin → destination. 30 position requests, no failures,
2.2–3.5 s each; 21 of 22 route lookups found a route (the one failure was
during the restart at the end of the window). Lowest free heap 34 KB (largest
block 20 KB: switched after the heap had fragmented); feed task stack 3.5 KiB
of 9 KiB never used. The owner saw logos arrive a second or two late, or not
before the next flight: a request held the block most of the time.

Logo fix, test build `edf61335f6e9`, community from boot: 63 logos, **59
decoded ahead** and 4 when the flight changed; the owner found them "snappy".
65 position requests, the only failure the last one, during the restart at
the end; route lookups all answered. Lowest free heap 32 KB, largest block
43 KB at the start. Both runs: no crash, stock came back unchanged.

Credit: flight positions from adsb.fi (personal, non-commercial use); routes
from adsbdb (aircraft data credited to PlaneBase, route data to David Taylor
and Jim Mason).

Stock app SHA-256 `0ab195d6…559f`. Tags: **[O]** seen in the decompile or data, **[I]** inferred, **[U]** unknown. Addresses are `FUN_…`. No secrets are reproduced; NVS items are given by name and length only (lengths from the review backup, parsed by a length-only script).

## On the board — 2026-10-19

Test build `owned-skeleton-v1-717160e6172b` (pending, rolled back after):
the stored configuration (1 865 bytes) gave a 972-byte request; the first
poll 6.3 s after boot and every 10 s after it answered 200 with a MessagePack
body of about 1.8 KB, 5 flights and 14–16 more reported, **no failures in the
whole window**. The panel showed the live flights with logos from the board's
own files (AAL, AJT, AVA, CJT, DAL, ENY, GTI, JBU, ROU, default), confirmed by
the owner (Delta and Avianca flights at the time). Heap low-water mark 88 KB
(a logo decode). No crash; stock came back unchanged. The owner noticed the
airport names were not aliased: aliases are an owned feature, added after
this run.

Aliases run, test build `owned-skeleton-v1-9ea84c94628c`: the table loaded
read-only (4 aliases, revision 4) before the first poll; polls all 200 with
five flights; the owner saw "Palm Beach" on the panel; no crash; stock came
back unchanged.

## 1. Flight request

### Transport

| Item | Value |
|---|---|
| URL | `http://api.theflightwall.com/v2/flights` (literal at `0x3c0f1e93`). This is the only feed URL; there is no https variant. **[O]** `FUN_42014fe0` |
| Scheme | Plain HTTP on port 80, through `WiFiClient`. The URL struct is hard-set to "http" (`uStack_100=1`), so the `WiFiClientSecure` branch is unused for this call. No TLS and no pinning. **[O]** |
| Method | `POST`, via `HTTPClient::POST(String)` → `sendRequest("POST")` (`FUN_4203b254` → `FUN_4203b23c`). **[O]** |
| App headers | `Content-Type: application/json`, `Accept: application/x-msgpack`, `x-api-key: <NVS api/key>` (`addHeader(…,first=0,replace=1)`). **[O]** |
| Library headers | Arduino HTTPClient defaults: `Host`, `User-Agent: ESP32HTTPClient`, `Connection: keep-alive` (reuse is on), `Accept-Encoding: identity;q=1,chunked;q=0.1,*;q=0`, `Content-Length`. **[I]** (strings present; library behaviour) |
| Not sent | Device id, firmware version, config hash, timestamps, signature or client cert. **[O]** |
| Timeouts | HTTP `setTimeout(10000)` (`FUN_42039014`), connect timeout 10000 (`FUN_420da098`), `client.setTimeout(10 s)`, body-read deadline 9999 ms of no progress with 10 ms polls (`FUN_42014d68`). **[O]** |
| Other client setup | One field (+0xa0) is set to 1, probably `setFollowRedirects(STRICT)` **[I]**. `collectHeaders({"Content-Type"})`. |
| Connection reuse | Keep-alive socket. It is closed before a request if there have been ≥2 consecutive failures or more than 60 000 ms since the last success. The 30-minute age limit applies only to TLS. **[O]** |
| Mutex | All vendor HTTP requests hold one global mutex (`FUN_4200c744`/`FUN_4200c78c`). **[O]** |

**API key.** `FUN_4202cac4` reads `Preferences("api", ro)` → `getString("key")`. The value is cached at http-object+0x1420, with a loaded flag at +0x1430. If the key is empty, no request is made and the attempt counts as a failure. **[O]**

### Body

The body is JSON (ArduinoJson 7). It is serialized from the request model (`FUN_4201325c`) and cached. It is rebuilt only when the model is dirty (`FUN_420149f4`). **[O]**

Members, in this order:

| Key | Type | Emitted when |
|---|---|---|
| `radius_request` | zone object | a radius zone exists (= first of `radius_requests`) |
| `geo_request` | geo object | a geo zone exists (= first of `geo_requests`) |
| `radius_requests` | array ≤5 of zone | non-empty |
| `geo_requests` | array ≤5 of geo | non-empty |
| `data_filters` | object | config had `data_filters` |
| `tracked_flights` | array ≤8 | non-empty |

The singular and plural forms are kept in sync by `FUN_420056f4`:
- If only the singular exists, it is pushed into the array.
- If only the array exists, its first element becomes the singular.

So both forms are always sent. **[O]**

**Zone** (`FUN_42013160`), keys in order:
- `type` `"radius"`
- `latitude`, `longitude`, `radius_km` (double; defaults 0, 0, 50.0)
- `min_altitude`, `max_altitude` (int32, only when ≥0; units **[U]**)
- `name` (only when non-empty)

**Geo** (`FUN_42013030`), keys in order:
- `type` `"geo"`
- `coordinates`: an array of `[n0, n1]` double pairs, passed through in config order. Whether the order is lat,lon or lon,lat is **[U]**.
- `min_altitude`, `max_altitude`, `name`, with the same rules as a zone.

**data_filters**, keys in order:
- `show_light_ac`, `show_heli_ac`, `show_commercial_ac`, `show_smalljet_ac`, `show_other_ac`, `show_na_routes`: bool, default **true** (`FUN_42002eb8`).
- `min_speed_mps`, `max_speed_mps`: int32, only when ≥0. Config values below −1 are clamped to −1.
- `airline_icao_filter`: array of exactly-3-char uppercased codes, at most 32. Sent only when non-empty; empty entries are skipped (`FUN_42003bdc`).

**tracked_flights[]** (`FUN_42003d24`), keys in order:
- `flight_number`: str, ≤11 chars, only when non-empty.
- `created_at`: str, ≤27 chars, only when non-empty.
- `auto_remove` (default true), `show_distance_travelled`, `show_metrics`: bool, always sent.

### Poll loop (`FUN_4201563c`) **[O]**

- **Fetch condition:** Wi-Fi is up, the previous response has been consumed by the display task, and the request-model semaphore is taken (5000 ms wait).
- **After a fetch:** waits `ulTaskGenericNotifyTake(0, pdTRUE, 10000)`, i.e. **10 s**, or less when notified. After the very first pass it waits 100 ms instead.
- **Settings change:** `FUN_42012630` copies the model, increments a generation counter and calls `xTaskNotify(…, eIncrement)`. That forces an immediate re-poll and a body rebuild.
- **Wi-Fi reconnect:** resets the client, waits 3000 ms, then polls.
- **Sleep:** the worker is stopped by its stop flag (+0x1c85). There is no polling **[I]** (consistent with PROVIDER-INTEGRATION).

### Errors **[O]**

- **Success** requires HTTP status **exactly 200**. 401, 403, 429 and 5xx are not told apart from each other or from transport errors.
- **No backoff and no Retry-After:** the next poll is still 10 s later.
- **On any failure** (`FUN_4200d9c4`):
  - the failure counter goes up;
  - the API host is re-resolved by DNS;
  - if DNS fails, the station DNS is switched to 8.8.8.8 / 1.1.1.1 (`FUN_4200d87c`), then reverted to DHCP DNS 30 s after a later success (`FUN_4200d960`).
- **Every 3rd consecutive failure** (3, 6, 9…): the HTTP client and connections are reset (`FUN_4200dc74`).
- **Display on failure:** a failed response is not shown; the display keeps the last good list (`FUN_4200f3e4` needs ok==1).

## 2. Response

### Reception (`FUN_42014d68`) **[O]**

- **Length:** needs a Content-Length of **1..5120 bytes** (`getSize()`). Chunked or larger bodies fail.
- **Format choice:** if `Content-Type` starts with `application/x-msgpack`, the msgpack parser `FUN_4200d594` is used. Anything else goes to ArduinoJson `deserializeJson` (nesting 10), then `FUN_420117e4`.
- **Buffer reset:** before parsing, the feed buffer is cleared (`FUN_4200b150`): counts, ch, ov, tracked flag and sync data.

### Feed buffer (0xd80 bytes, double-buffered) **[O]**

| Off | Field |
|---|---|
| 0x000 + i·0x200 | flight records i < 5 (only 0x1f9 bytes are used) |
| 0xa00 | u8 count |
| 0xa01 | `ch` char[40] (**39 chars max**) |
| 0xa2c | `ov` (msgpack stores 16 bits) |
| 0xa30 | tracked flag |
| 0xa34 / 0xa94 | `sla` / `slu` legs (see SYNC.md) |
| 0xc74 / 0xc75 | `slu` count / `sla` present |
| 0xc76 | `scm` [256] |

### Top level

| msgpack | JSON | Rule |
|---|---|---|
| (root) | (root) | must be a map/object, else the response fails |
| `v` | — | integer; > 1 → the response fails |
| `f` | `flights` | array; records appended, max 5, extras skipped; sets tracked = 0 |
| `tf` | `tracked_flights` | sets tracked = 1. In JSON, if present, `flights` is ignored. In msgpack both append, and the last key seen decides the flag |
| `ch` | `config_hash` | string, cut to 39 bytes |
| `ov` | `overflow` | **integer only** (a msgpack bool fails the response) |
| `sla`, `slu`, `scm` | `sync_leg_active`, `sync_legs_upcoming`, `sync_custom_message` | see SYNC.md |

Other msgpack rules **[O]**:
- Top-level keys are read into a 4-byte buffer, so at most 3 chars are compared. Record keys get 12 bytes; `deltas`/`trk_info` sub-keys get 8.
- Unknown keys are skipped (`FUN_4200cb7c`, depth ≤16). An unsupported type sets the error flag.
- **Any type error anywhere rejects the whole response.**
  - The integer reader takes positive fixint, u8–u64, i8–i64 and nil. It rejects **negative fixint** and floats.
  - The double reader takes f64, f32, ints, and nil→NaN.
  - The string reader takes str8/16/32/fixstr, and nil→"".
  - The bool reader takes true/false/nil and positive fixint.

JSON: wrongly typed fields are ignored one by one. There is no `v` check. **[O]**

### Flight record (`FUN_4200cee0` msgpack, `FUN_42010604` JSON; init `FUN_4200778c`) **[O]**

Both encodings use the **same short keys**. Units come from the template engine (fw_flight.h / FLIGHT-SCREENS.md) **[I]**.

| Key | Off | Type / limit | FwFlight / token |
|---|---|---|---|
| `icao` | 0x00 | str 7 | `icao` |
| `cs` | 0x08 | str 11 | `cs`, `{cs}` |
| `reg` | 0x14 | str ≤15 after trim; only `[A-Za-z0-9-]`, else left empty; upper-cased (`FUN_420d8d2c`); input read ≤31 | `reg`, `{reg}` |
| `alt_b`, `alt_g` | 0x28, 0x30 | double, m, NaN if absent | `{alt}` |
| `vel` | 0x38 | double, m/s | `{vel}` |
| `trk` | 0x40 | double, ° | `{trk}` |
| `vr` | 0x48 | double, m/s | `{vr}` |
| `gnd` | 0x50 | bool | `gnd` |
| `sqk` | 0x51 | str 7 | `{sqk}` |
| `orig_icao`, `orig_iata`, `dest_icao`, `dest_iata` | 0x59/61/69/71 | str 7 | `{orig_iata}` `{dest_iata}`… |
| `orig`, `dest` | 0x79, 0x99 | str 31 | `{orig}` `{dest}` |
| `al_code`, `al_paint`, `al_op` | 0xb9/c1/c9 | str 7 | logo = `al_code`; `{al_code}` |
| `al_name` | 0xd1 | str 23 | `{al_name}` |
| `ac_type` | 0xe9 | str 15 | |
| `ac_name` | 0xf9 | str 31 | `{ac_name}` |
| `ac_class` | 0x119 | str **15** (FwFlight has 24) | |
| `deltas{alt_b,alt_g,vel,trk,vr,dt}` | 0x130–0x158, flag 0x160 | doubles; use **[U]** | not in FwFlight |
| `trk_info.sts` | 0x1f0 | exactly `"C"` or `"D"` | `sts` |
| `trk_info.sexp` | 0x1f4 | u32 epoch s, integer 1..2³²−1 (msgpack must be an int type; JSON accepts it only together with a valid `sts`) | `sexp` |
| `trk_info.prog` | 0x168 | double, % | `prog`, `{trk_prog}` |
| `trk_info.dist` | 0x170 | double, m | `dist`, `{trk_dist}` |
| `trk_info.conf` | 0x178 | str 15 | `{trk_conf}` |
| `trk_info.dep` / `arr` | 0x188 / 0x1a0 | str 23 | `{trk_dep}` `{trk_arr}` |
| `trk_info.over` | 0x1b8 | str 31 | `{trk_over}` |
| `trk_info.tft` | 0x1d8 | str 23 | `{trk_tft}` |
| `trk_info.gnd` | 0x1f1 | bool | `trk_gnd` |
| (any trk_info key) | 0x1f8 | has_trk | `has_trk` |

How records are kept or dropped:
- **Strings** are cut bytewise to size−1. Mid-UTF-8 cuts are possible; only `scm` is repaired.
- **No per-record rejection:** a record with every field empty is still stored.
- **Dropped records:** only those beyond 5.
- **Whole-response loss:** in msgpack, any field with a bad type (also a bad `ov`/`v`, or a non-map root) loses the whole response.

## 3. Configuration sync

### Trigger (`FUN_42015fb8` → `FUN_42015b34`) **[O]**

The display task runs this check on every response whose `ch` is non-empty. It does nothing when:
- `ch` equals the stored `usercfg/config_hash` (`FUN_4202cd08`); or
- `ch` is all `'0'`; or
- the last attempt was less than 30 000 ms ago.

Otherwise:
1. Shows "Configuring...".
2. `GET /configuration` (`FUN_42022e08`).
3. Stores the body, re-parses the stored copy (`FUN_42005fb4`), applies the display settings, rebuilds the request model and notifies the worker.
4. Only then saves `config_hash` (`FUN_4202cdd0`).

**On failure** the hash is not stored, so the fetch is retried every 30 s while the mismatch lasts.

### Endpoint (`FUN_42022cf4`/`FUN_420227a4`) **[O]**

- **URL table:** `{http://…/configuration, https://api.theflightwall.com/configuration}`; HTTPS is used by default.
- **Method:** `GET`, no body.
- **Headers:** `x-api-key`, `Connection: close` (reuse off). Timeouts are 10 s.
- **TLS:** `WiFiClientSecure` with one CA set via `setCACert`: **GTS Root R4** (DER 525 bytes). This is a root-CA pin, not a leaf pin.
- **HTTP fallback:**
  - After 2 consecutive negative (transport/TLS) results, requests use the plain-HTTP URL for the next 20 requests.
  - A negative first attempt is retried at once over HTTP.
  - A force-HTTP flag (global +4) exists; what sets it is **[U]**.
- **Response handling:**
  - 200 with an empty body becomes `{}`.
  - The root must be an object with ≥2 members.
  - `request_config` and `display_config` objects are checked.
  - The body is minified and saved with `putBytes("usercfg","json")`; it must be **≤4096 bytes** (`FUN_4202d564`).
  - Applying needs both `request_config` and `display_config`.

### Schema (`FUN_42005fb4`)

- `version`: 1..255, logged as "Schema version v%u loaded (firmware supports v2)". **[O]**
- `request_config` (`FUN_420057c0`): `radius_request`, `geo_request`, `radius_requests`, `geo_requests`, legacy top-level `type`+fields (`FUN_42004598`), `data_filters`, `tracked_flights`. Fields are as in §1. Zone `id`, and `type` inside arrays, are ignored. **[O]**
- `display_config` (`FUN_420189b4` unless noted). **[O]**

  | Key | Values / default |
  |---|---|
  | `layout_template` | `DEFAULT` |
  | `loading_template` | `TEXT` |
  | `title_text` | `airline` \| `callsign` \| `registration` |
  | `locale` | `en` |
  | `date_format{order,style,show_weekday}` | |
  | `anchor_airports[]` | |
  | `flight_display_speed` | `slow` 5000 / `fast` 2000 / else 3000 ms |
  | `hide_max_flights_screen` | |
  | `max_flights_behavior` | `off` / `text` / `indicator_led` |
  | `metric_display` | `alt_b_unit` (meters/feet/kilometers/nautical_miles), `vel_unit` (kmh/mps/knots/mph/mach), `vr_unit` (kmh/mps/fps/fpm/mph), `trk_unit` (`cardinal`), `dist_unit` (…/miles) |
  | `time_zone{standard_utc_offset_minutes,dst_rule}` | |
  | clock flags | `clock_utc_offset_minutes`, `clock_dst_enabled`, `clock_show_seconds`, `clock_is_24hr` |
  | `clock{style DEFAULT\|LARGE, show_date, additional_timezones[{label,…}]}` | see CLOCK.md |
  | `brightness_percent`, `border_enable`, `border_brightness_percent` | `FUN_4202252c` |
  | `display_enabled`, `sleep_time{start,end,sleep_brightness_percent,per_day,time_zone,utc_offset_minutes}` | `FUN_4201c69c` |
  | `model` | read, but the runtime forces `mini-v1` **[I]** |

- `plus.sync.*`: `FUN_42003fa8`, see SYNC.md.
- A real stored config on this board matches this shape (`analysis/source-firmware/*/before.json`; key names only were inspected).

### Boot and connect **[O]**

- **Boot, before network:**
  - `FUN_42006368` parses the stored `usercfg/json` and applies `display_config`.
  - `FUN_42015a14` loads `request_config` into the worker and notifies it.
- **On Wi-Fi up** (`FUN_420063e4`, state 5):
  - `GET /configuration` runs **unconditionally**: once per connect, at most every 5000 ms, up to 3 tries 2 s apart.
  - This path does **not** store the hash.
  - If the stored config lacks `request_config` or `display_config`, the panel shows "Configure settings / in the app to / display flights..." and `/configuration` is polled about every 10 s (`FUN_4201cc88`) **[I]**.

## 4. Credentials and storage

| NVS ns/key | Type | Length (backup) | Source |
|---|---|---|---|
| `api/key` | string | 44 incl. NUL (43 chars) | Factory: serial line protocol `FWFACT {"cmd":"set_api_key","api_key":…}` (`FUN_42025054`→`FUN_420247b0`→`FUN_4202cb8c`). Written only if absent; replies `stored` / `already_present` / `api_key_locked` / `save_failed`; gated by a factory-provisioning check (`FUN_4202bbf4`). Other cmds: `ping`, `status`. **[O]** A boot consumer of a `factoryprov` partition may also seed it **[I]** (VENDOR-UPDATE-AUDIT). |
| `usercfg/json` | blob | 1767 (≤4096) | `/configuration` |
| `usercfg/config_hash` | string | 40 incl. NUL (39 chars) | `ch` after a successful adopt |
| `identity/id`, `fp`, `cert_der`, `priv_der`, `provisioned` | str/str/blob/blob/u8 | 7 / 21 / 497 / 165 / 1 | setup HTTPS server and QR. Not used by the feed; `id` is the `device_id` in status reports **[O]** |

**Pairing.** Nothing is fetched from the cloud for credentials. The phone app reads `api_key` from the AP-local `GET /status` once the device has joined Wi-Fi (`FUN_42020ab8`, SETUP.md) **[O]**. The key itself never changes on the device; no renewal flow was found **[I]**.

## 5. Other vendor and background traffic

| Call | When | Sends | Owned fw |
|---|---|---|---|
| `POST https://api.theflightwall.com/devices/status` (`FUN_42017608`; body `FUN_42017460`) | 5 s after first connect, then every 600 s (`FUN_42017e74`); also on state changes (`FUN_42017ed0`) | JSON `device_id`, `app_state` (ConfigPolling/FlightDisplay/SleepMode/OTAUpdate/…), **`ssid`**, `uptime_seconds`, `firmware_version` "4.0.3", `mac_address`, `diag{free,min_free,largest,reset,last_restart,fetch_stack_min,frag_events}`. Headers: `Content-Type`, `x-api-key`, `Connection: close`. HTTPS only, GTS R4; response ignored | Optional telemetry. Skip it (it leaks the SSID and MAC). Effect of omitting it on the backend is **[U]** |
| `GET /configuration` | §3 | — | Optional; local settings can stay authoritative |
| `cdn.theflightwall.com/{firmware,filesystem}/mini-v1/manifest.json` (https, then http) | about weekly (`0x42016fd8`), signed (`ota-prod-2026-03`) | — | Do not reproduce (blocked in owner builds) |
| SNTP `pool.ntp.org`, `time.google.com`; `GET http://clients3.google.com/generate_204` `Date` header fallback (`FUN_4202c504`/`FUN_4202bd9c`) | network check | — | Non-vendor |
| DNS check of `api.theflightwall.com`, fallback 8.8.8.8/1.1.1.1 | network test | — | Non-vendor |

## Minimum owned implementation

1. Send `POST /v2/flights` with the three headers and the body in §1, built from local settings.
2. Poll every 10 s, and re-poll at once when settings change.
3. Accept only a 200 response with a Content-Length of at most 5 KiB.
4. Parse msgpack (`v`, `f`/`tf`, `ch`, `ov`, sync keys) with the stock type rules; an integer `ov` is required.
5. Keep the last good list on any failure. Adding backoff for 429/5xx is a safe improvement over stock.
6. HTTPS to the same host also worked from a desktop (LOCAL-AUTHORITY-AND-DATA-SOURCES.md) **[O, external probe]**.
7. `ch` may be ignored.

## Unknowns

- Altitude units in the request.
- Coordinate order in `coordinates`.
- What `deltas` are used for.
- What sets the force-HTTP flag.
- Server behaviour with no status reports or a stale hash.
- Server-side 401/429 semantics.
