# Source-owned flight data core

This is portable C++17 source for the replacement runtime. It currently provides
provider decoding, route enrichment, local logo resolution and polling policy.
It is **not a flashable firmware**. The installed development profile binds its
existing adapters to the stock flight worker and renderer; the new source-built
[HTTPS component](net/README.md) is a separate prototype and is not installed.
The runtime has no mandatory PC/server in its
design; Python and the command-line inspector below are development tools only.

## Implemented

- Vendor JSON/MessagePack (`f`/`flights`) and adsb.fi JSON (`ac`/`aircraft`) adapters.
  Both produce fixed-size owned flight records with no pointers into input JSON.
- Input limit 128 KiB, nesting limit 12, caller-owned parsing memory, 32 flights
  maximum, duplicate tracking and overflow reporting. Missing or malformed
  envelopes fail; invalid individual rows are counted and skipped.
- Community position age/coordinate checks, explicit missing metrics, feet/knots/
  feet-per-minute conversion to SI, and preserved route provenance. Ground state,
  zero coordinates, zero speed and north heading are not confused with missing
  values. The stock renderer's conversions establish vendor `alt_b` as metres,
  `vel` and `vr` as metres/second, and `trk` as degrees.
- ADSBDB route enrichment matches the requested ICAO callsign. It cannot replace
  vendor routes or known endpoints, and is identified as a callsign-database
  route rather than a live schedule/ETA.
- Shared logo lookup: explicit airline ICAO, vendor `al_code`, unique IATA
  crosswalk, then a known locally available callsign prefix. Owner artwork wins
  over stock artwork. Unknown/private aircraft use a category/default icon or
  text when no artwork exists. Only validated three-letter codes form paths.
  `al_paint` and `al_op` are retained without inventing precedence.
- Fixed HTTPS endpoint plans for both providers, key separation, known vendor
  request-field projection, five-second polling reservation shared across zones,
  exponential backoff/Retry-After handling, retained last-good frames and explicit
  source changes. There is no silent switch to another provider.
- Incremental community reader validates the complete response while retaining
  only one aircraft JSON object at a time. Tests process 150 aircraft using a
  4 KiB row buffer and a 4 KiB JSON document, plus the fixed staging batch.
  Truncated bodies, IO failure and malformed trailing metadata clear staging.
- Stock display wire adapter emits MessagePack `v:1`, at most five rows and an
  overflow flag, with shared airline-logo codes, metric SI values and raw airport
  identifiers for the existing alias hook. It excludes cloud configuration,
  update and tracked-flight metadata. A supplied decoder callback runs only
  after complete successful encoding; the adapter never retains its output.

`FrameState` publishes only successful staging frames from the selected provider.
The caller owns active/staging storage and must serialize access. Parsing does
not read settings, store credentials, write flash, fetch images, or execute
configuration/update metadata returned with flight data.

## Build and tests

The pinned MIT-licensed ArduinoJson 6.21.5 amalgamated header is included with its
license, upstream commit and SHA256. No package download is needed to build.

```
py -m unittest discover -s runtime/tests -v
cmake -S runtime -B runtime/build
cmake --build runtime/build
ctest --test-dir runtime/build --output-on-failure
```

The tests use synthetic data and perform no network/device accesses. They cover
JSON/MessagePack truncation, malformed/envelope/size/memory limits, units, stale
positions, routes, logo equivalence and fallbacks, credential separation, polling
backoff/clock wrap, and explicit provider selection without mixed-source frames.
The portable sources also compile with the installed ESP32-S3 Xtensa compiler.

Developer-only offline inspector:

```
feed_inspect adsb-fi PRIVATE_RESPONSE.json [PRIVATE_LOGO_INVENTORY.json]
feed_inspect vendor-msgpack PRIVATE_RESPONSE.msgpack [PRIVATE_LOGO_INVENTORY.json]
```

It prints counts and memory usage, not aircraft identifiers or credentials.
Do not commit captures, device credentials or extracted proprietary artwork.

## Verified live inputs, 2026-10-18

Independent HTTPS requests using this owner's existing vendor device key and a
public FLL community query succeeded. Captured bytes were then decoded by this
C++ implementation, using the real on-device asset inventory as the logo catalog:

| Input | Accepted flights | Airline images | Category images | Text-only |
| --- | ---: | ---: | ---: | ---: |
| adsb.fi around FLL | 26 | 19 | 7 | 0 |
| Vendor HTTPS feed | 5 | 5 | 0 | 0 |

The vendor response reports overflow; the core preserves that flag. Route-service
HTTPS probing also confirmed the documented airport and airline response shape.
These are captured-input/source tests, not proof of the new core running on ESP32.

The preserved LittleFS contains 496 airline PNGs (492 at 64×64, four at 60×60),
four 64×64 category/default icons, and a system logo. All 501 PNGs passed chunk
CRCs, image verification and full decode with the filesystem and source hashes
unchanged. Existing stock `al_code` maps to record offset `0xb9`, which its logo
resolver uses; category fallback uses `ac_class` at `0x119`.

## Current integration and remaining acceptance work

`community_config.*` validates radius/altitude/speed/category/airline settings;
`community_cache.*` bounds nearest retention, merges zone results, preserves
matched routes and expires stale positions. `esp32/http_body.*` bounds HTTP
framing. The installed development profile binds these modules to verified HTTPS
and the stock display worker, with separate revisioned NVS source selection.
Target capacity is five flights and 2 KiB row +2 KiB JSON scratch with memory
guards; portable capacity defaults to 32. An extra-task prototype failed memory
checks and was removed. The selected source remains vendor and no dashboard
selector is exposed yet.

October 19 hardware testing found that the stock `ov` reader requires an integer;
both normal frames and the source-change empty frame now encode numeric overflow.
The stock worker then accepted live community flights and airline-logo codes.
LAN buffers are allocated per request and serialized with community processing.
Route HTTPS still exceeded a safe memory peak; it now requires 96 KiB free heap
and is normally deferred on this stock build. Testing positions/logos temporarily
enables `show_na_routes` (unknown routes), then restores the original filter.
Do not claim community airport-route parity or expose it as a finished feature.

1. Resolve route-memory requirements and verify airport-route rendering on an
   awake panel; inspect final worker memory/stack through repeated position and
   route requests. Community flight rows and airline images were directly
   confirmed by the user on October 19.
2. Test switching/restart persistence, stale expiry, Internet loss/recovery and
   Wi-Fi rollback. Host tests are not device memory/stack proof.
3. Extend unsupported tracked-flight/geofence semantics and incomplete aircraft
   classification. Show limits and service attribution in the eventual selector.
4. Establish an
   isolated owner logo store plus validated atomic upload/preview/delete APIs.
   No stock artwork or filesystem is embedded/overwritten by this source tree.

Service references: [adsb.fi API/terms](https://github.com/adsbfi/opendata/blob/main/README.md),
[readsb field units](https://github.com/wiedehopf/readsb/blob/dev/README-json.md),
[ADSBDB](https://www.adsbdb.com/),
[ArduinoJson JSON](https://arduinojson.org/v6/api/json/deserializejson/) and
[MessagePack parsing](https://arduinojson.org/v6/api/msgpack/deserializemsgpack/).
Provider policies/availability can change; production bindings must display
attribution, honor limits and avoid claiming complete route/schedule coverage.

Device integration evidence and the current memory budget are recorded in
[PROVIDER-INTEGRATION.md](../PROVIDER-INTEGRATION.md). These source adapters are
connected through the installed development hook but are not the selected feed.
On October 19 the user directly confirmed community flight rows and airline
images on the panel. Airport-route rendering remains untested because route
lookups are deferred by the memory admission guard.
