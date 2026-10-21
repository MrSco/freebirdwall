# Owned settings

The owned firmware keeps its own copy of the wall's settings and lets you
change them from the dashboard (`http://<wall address>:8080/`, see
[DASHBOARD.md](DASHBOARD.md)) instead of the vendor app.

## Imported once

The settings document is the vendor app's own JSON (`version`,
`request_config`, `display_config`, `plus`, ...), so the feed and the screens
read it exactly as they read the vendor's copy (FEED.md).

- The first time the owned firmware starts with no owned copy, it reads stock's
  stored copy (`usercfg/json`, read only) and saves it as the owned one at
  revision 1 (`main/settings_store.c`). A board with no vendor copy starts
  from `{"version":2,"request_config":{},"display_config":{}}`.
- From then on the dashboard changes it, and so do changes made in the
  FlightWall app (below).
- Stock never reads the owned copy, so switching back to stock shows the
  vendor app's settings again.

## Changes made in the app

The wall asks the vendor for the app's settings as stock does
(`GET https://api.theflightwall.com/configuration`, `x-api-key`, GTS Root R4
pinned; FEED.md §3) and merges them in **one setting at a time**
(`components/fw_settings/fw_cloud.c`, `main/app_settings.cpp`):

- It keeps a fingerprint of every setting as the app last had it
  (`fw_owned/cloud_base`, 520 bytes). A setting whose fingerprint moved was
  changed in the app: its value replaces the owned one (or removes it, when
  the app removed it). Everything else, including what was changed only on
  the dashboard, stays. A setting changed in both places: the later app
  change wins.
- A setting is a member of `display_config`, `request_config` or `plus`, or a
  top-level value (`version`). The request's areas (`radius_request`,
  `radius_requests`, `geo_request`, `geo_requests`, `type`) count as one,
  since they replace each other. `owned_config` is never taken from the app.
- The first configuration ever seen only sets the fingerprints, so older app
  values never undo what the dashboard already changed.
- Like stock, only a configuration with `request_config` and
  `display_config` objects, at most 4096 bytes, is used.
- **When** (never on a blind timer): on the vendor feed, as stock, when an
  answer's `config_hash` (`ch`) differs from the last one handled (empty or
  all `0` is ignored; retried every 30 s while it fails). The vendor sends no
  other signal, so on the community source, or while the vendor feed is not
  answering: once per Wi-Fi connection (stock fetches on every connect;
  retried every minute until it works) and when the dashboard is opened or
  comes back to the front (at most every 5 minutes). A check made just before
  the vendor feed's first answer covers that answer's hash.
- **Flash:** only when a setting changed in the app (the owned settings, then
  the fingerprints), and the fingerprints once the first time. A check that
  finds nothing new writes nothing, as with stock (which stores
  `usercfg/json` and `config_hash` per app change).
- **Device › FlightWall app** turns it off (`owned_config.follow_app`
  false) and has **Copy everything from the app**: the app's whole document
  replaces the owned one, except `owned_config`; airport names and Sync
  flights are stored apart and stay.
- Logged as `app_settings` (counts, HTTP status, how many settings changed;
  never the document or the key).

Storage (NVS namespace `fw_owned`, never a stock namespace):

| key | contents |
|---|---|
| `config` | the settings document (blob, at most 4096 bytes) |
| `config_rev` | its revision (u32, +1 on every save) |
| `pair` | the signed-in browsers (below) |
| `games_rot` | the games switch (DASHBOARD.md) |

Airport names stay in `fw_owner/aliases` (FEED.md, "Airport aliases").

## What can be changed

`components/fw_settings` checks every edit before it is saved; anything it
does not know is refused, with the field and the reason. An edit is
`{"display_config":{...},"request_config":{...}}` with any subset of these
keys. Each key given replaces the stored value; `null` removes it (not
`radius_requests`); everything else in the document is kept (`plus`,
`version`, geo zones, keys the dashboard does not edit). The section
`owned_config` is the owned firmware's own (below).

`display_config`

| key | values |
|---|---|
| `layout_template` | `DEFAULT`, `NO_LOGO`, `TECHNICAL` |
| `loading_template` | `TEXT`, `CLOCK`, `ELLIPSIS`, `BLANK` |
| `title_text` | `airline`, `callsign`, `registration` |
| `flight_display_speed` | `slow`, `normal`, `fast` |
| `max_flights_behavior` | `text`, `indicator_led`, `off` |
| `metric_display` | `alt_b_unit` meters/feet/kilometers/nautical_miles, `vel_unit` kmh/mps/knots/mph/mach, `vr_unit` kmh/mps/fps/fpm/mph, `trk_unit` cardinal/degrees, `dist_unit` meters/kilometers/miles/nautical_miles/feet |
| `brightness_percent` | 1..100 |
| `border_enable`, `border_brightness_percent` | bool, 0..100 |
| `anchor_airports` | up to 8 codes, 3–4 of A–Z 0–9 |
| `time_zone` | `standard_utc_offset_minutes` −720..840 in 15s, `dst_rule` none/north_america/europe |
| `clock_is_24hr`, `clock_show_seconds` | bool |
| `clock` | `style` DEFAULT/LARGE, `show_date`, `additional_timezones` (≤2 of `{label ≤15, time_zone {standard_utc_offset_minutes, dst_rule}}`, the vendor's shape) |
| `date_format` | `order` month_day/day_month, `show_weekday` |
| `locale` | en es fr de it pt nl pl sv no da fi cs ro hu el tr uk ru |
| `display_enabled` | bool |
| `sleep_time` | `start`, `end` "HH:MM", `sleep_brightness_percent` 0..100 |

`request_config`

| key | values |
|---|---|
| `radius_requests` | 1–5 areas: `latitude`, `longitude` (required), `radius_km` 1..250, `min_altitude`/`max_altitude` 0..20000, `name` ≤31. Saving it drops the old single `radius_request` and `type`. |
| `data_filters` | `show_light_ac`, `show_heli_ac`, `show_commercial_ac`, `show_smalljet_ac`, `show_other_ac`, `show_na_routes`, `min_speed_mps`/`max_speed_mps` 0..400, `airline_icao_filter` ≤32 of 3 capitals |
| `tracked_flights` | ≤8 of `flight_number` (2–11 A–Z 0–9), `auto_remove`, `show_distance_travelled`, `show_metrics`; `created_at` is kept for numbers already tracked and set to now for new ones |

`owned_config` (owned only; the vendor app and stock never read it)

| key | values |
|---|---|
| `flight_source` | `vendor` (default) or `community` (FEED.md, "Community feed") |
| `follow_app` | bool, default true: changes made in the FlightWall app reach the wall (below, "Changes made in the app") |
| `heartbeat_enabled` | Boolean, default true: blink the orange pixel at the bottom right; false hides it on every screen |
| `scroll_speed` | `slow`, `normal` (default) or `fast`: how fast names too long for their line scroll (FLIGHT-SCREENS.md "Long text") |
| `tracking_source` | `community` (default) or `aeroapi`: where tracked flights' times come from on the community source (FEED.md, "Tracked flights on the community source") |

A saved change reaches the wall at once: the feed rebuilds its request and
polls again, and the screens reload their settings. Brightness, display off
and the sleep window (in local time, may cross midnight) are applied about once
a second; a game or a sign-in code wakes the panel.

## Signing in

Reading or changing settings needs a signed-in browser.

1. **Show a code on the wall** (`POST /api/pair/start`, at most once per 5 s)
   — the wall shows "Dashboard code", six digits and a countdown for 120 s.
2. Type the code. Right: the browser gets a random 32-byte key (64 hex) and
   keeps it (`localStorage`). Wrong: 5 tries, then the code is cancelled.
3. Every settings request then sends `Authorization: Bearer <key>`.

The wall keeps only SHA-256 hashes of the keys, for at most 4 browsers (the
least recently used is dropped). **Sign out everywhere** forgets them all.
The update service's device key is accepted too, for desk tools. Codes, keys
and settings values are never logged.

## API (port 8080)

| route | |
|---|---|
| `GET /api/session` | `{"signed_in","code_showing","seconds_left"}` |
| `POST /api/pair/start` | `{"seconds":120}`; 429 if asked again within 5 s |
| `POST /api/pair/finish` `{"code"}` | `{"key"}`; 403 `{"error":"wrong code","tries_left"}`; 409 when no code is showing |
| `POST /api/pair/forget` | signs every browser out |
| `GET /api/settings` | `{"revision","settings","aliases":{"revision","aliases"},"feed"}` |
| `PUT /api/settings` | `If-Match: <revision>`, an edit (≤8 KB) → the same as GET; 400 `{"error","field"}`; 409 when the revision is stale; 428 without If-Match |
| `POST /api/settings/import` | queues **Copy everything from the app** → 202 with the status below |
| `GET /api/settings/import` | `{"pending","result","checked","changed_at","changed","status"}`: `result` never/ok/network/refused/not_saved, times in unix seconds (0 never since start); `?check=1` asks for a check (the page does on opening) |
| `GET /api/tracking` | `{"key_set","key_end","in_use","rejected","flights_calls","position_calls","last_status"}`: the FlightAware key's last four characters only, and its use since start |
| `PUT /api/tracking` | `{"aeroapi_key":"…"}` saves the key (8–64 of A–Z a–z 0–9 - _) in `fw_owned/aero_key`, `null` removes it |
| `PUT /api/aliases` | `{"revision","aliases":[{"kind","from","to"}]}` (≤16) → the same as GET; 400; 409 |

`feed` is 0 demo screens (no API key), 1 needs settings, 2 waiting, 3 live.

## The dashboard

`main/dashboard.html`, one embedded page, dark or light with the device setting,
built for a phone first: one column and a four-across tab grid on a phone, two columns
on a wide screen. Tabs: **Display**, **Clock**, **Flights** (areas, aircraft,
airlines, tracked flights), **Airports** (airport names, your airports),
**Stats** (flights seen: today, top airlines, 48 hours, 31 days; ANALYTICS.md),
**Games**, **Device** (feed state, the FlightWall app, console, sign out). Changes collect in a
bar at the bottom (**Save** / **Discard**); Save sends only the keys that
changed, trimmed to what the firmware accepts (vendor documents carry other
keys and use −1 for unset limits). A refused value is outlined and scrolled
into view. Speeds are shown in knots and stored in m/s. Games work without
signing in.

Saved watch areas start collapsed. Expanding an area loads Leaflet 1.9.4
(with integrity checks) from unpkg and tiles from OpenStreetMap in the browser.
Tap the map, drag the pin or explicitly search an airport/place (Nominatim;
no autocomplete). The selected point and radius use the existing
`radius_requests` settings API. Coordinates remain an optional fallback when
internet access is unavailable. Only the map and search need internet; the
rest of the dashboard works on the local network. The wall's pairing key is
never sent to those services.

Display includes a sample 128×64 preview, using the firmware's 5×7 font and
mini layout geometry. It reflects draft title, layout, units, brightness,
border, scrolling, flight dwell time, empty/overflow behavior, power, sleep
brightness and heartbeat. Logos and perceived LED brightness are approximate.
The heartbeat switch is owned-only and defaults on for existing installations.
Reduced-motion browser preferences stop preview animations.

Browser regression checks use Playwright with a Chromium executable (override
`CHROMIUM_PATH` if needed). Install Playwright in your development environment,
then cache the pinned map library (build outputs are ignored):

```sh
mkdir -p runtime/build/deps/leaflet
curl -fsSL https://unpkg.com/leaflet@1.9.4/dist/leaflet.js -o runtime/build/deps/leaflet/leaflet.js
curl -fsSL https://unpkg.com/leaflet@1.9.4/dist/leaflet.css -o runtime/build/deps/leaflet/leaflet.css
node runtime/firmware/tests/test_dashboard.mjs
```

These checks use local wall, tile and search fixtures; no requests reach the
public map services during the tests. Screenshots are saved under
`runtime/build/dashboard-tests/`.

Tested in a browser against a stand-in server that runs the real validator
(`tests/settings_dump.c`): sign-in with a wrong then a right code, every tab
at 375 px and on a wide screen, and saves of layout, sleep off (`null`),
your airports, airport names and aircraft filters, all accepted.

## On the board — 2026-10-19

First test build (`307925ba43a1`): asserted on the first frame
(`xQueueSemaphoreTake`, `pxQueue`). The panel asks for the sign-in code
every frame from boot, and the store's lock only exists once Wi-Fi is up.
It rolled back to stock with nothing changed. Fixed in `c91446c`: every store
entry point answers "not ready" until it is loaded.

Second test build (`e74c0a03712b`), pending, rolled back by itself:

- `settings_imported` at 6.6 s (revision 1); airport names loaded (4); the
  feed configured from the owned copy and polled with no failures.
- `GET /` 200 (44.9 KB); `/api/settings` without a key 401; with the device
  key revision 1, feed live.
- From a phone: the code showed on the wall, sign-in worked; brightness
  changed on the wall within a second of saving; layout changes saved
  (4 × `settings_saved`, each followed at once by `feed_configured`).
- The feed task's stack fell to 308 bytes free of 4 KiB once settings were
  re-read on it; raised to 6 KiB.
- After rollback, stock's firmware, airport names and the vendor app's
  settings were identical to the snapshot: edits stay in `fw_owned`.
