# Weather when the sky is quiet — vendor 4.1/4.2, and the owned source

Vendor firmware 4.1 added METAR weather to the screen shown when no flights
are nearby. The rules below were recovered from vendor app 4.2.4
(`6f521120…`, Ghidra 12 headless decompile; `FUN_…` addresses are in that
image). Code: `components/fw_weather` (portable C: record, formatters,
layouts, drawing, owned icons), `main/weather.cpp` (the board's record and the
owned aviationweather.gov source), `main/display.cpp` (`weather_screen`).
Tests: `tests/test_weather.c`, `tests/test_feed.py` (`test_42`), the
`weather_*` scenarios of `tests/test_preview.py`.

## Settings

| Key | Values | Default |
| --- | --- | --- |
| `display_config.weather.enabled` | bool | false |
| `display_config.weather.layout` | `summary`, `simple`, `technical` | summary |
| `display_config.metric_display.temp_unit` | `celsius`, `fahrenheit` | celsius |
| `….wind_unit` | `knots`, `mph`, `kmh`, `mps` | knots |
| `….vis_unit` | `miles`, `kilometers`; anything else: statute miles for K/P stations, else km | auto |
| `….ceil_unit` | `feet`, `meters` | feet |
| `owned_config.weather_station` (owned) | 3–4 letter ICAO, `""` = nearest | nearest |

The weather replaces the clock: it shows only with `loading_template`
`CLOCK` (`FUN_42014dfc`) and a record at most 3 h old. The dashboard's
"When the sky is quiet › Weather" sets both. (Stock also drops the clock's
extra time zones while weather is on; the owned firmware keeps them.)

**Between flights (owned):** `owned_config.weather_in_rotation` (and
`clock_in_rotation` for the clock) puts the weather in the flights' rotation:
once a pass through the flights, at least every 20 s, at a card's end, for a
flight's time but at least 6 s, the weather first when both are on; then the
rotation carries on with the flight that was due. Only with a fresh report;
never during tracked flights, Sync or a game. It fetches the weather even when
the "sky is quiet" screen is not the weather (`main/display.cpp` "breaks").

## Where the record comes from

- **FlightWall feed:** the response's top-level `weather` map
  (`FUN_4201085c`; keys and value rules in `fw_weather.h`). The device asks
  for nothing: the server decides from the app's settings. A response with
  flights keeps the record held; one without replaces it, or clears it when it
  has none (`FUN_4200eecc`). Wrongly typed values are ignored.
- **Owned, aviationweather.gov:** on the community source, and on the
  FlightWall feed when weather is on but the feed has sent none for 30 min,
  the wall reads `GET https://aviationweather.gov/api/data/metar?ids=<stn>&format=json`
  every 10 minutes (2 on failure), DigiCert Global Root G2 pinned
  (`main/awc_root.h`). With no station set it finds the reporting station
  nearest the first area once (`bbox` around it, about 65 km, then 220 km)
  and again after three empty answers or when the area moves.
  `fw_weather_from_awc` turns the JSON into the vendor's record: category as
  given; condition from `wxString` (TS thunder, SN/SG/PL/GS snow, RA/DZ rain,
  FG/BR/HZ/FU fog) else the most covering cloud layer (OVC/OVX overcast, BKN
  cloudy, SCT partly cloudy, else clear); ceiling the lowest BKN/OVC/OVX
  base; `10+` visibility as 10+; night from the sun's height at the station
  when observed; the name before its first comma, whole words up to 24 bytes.
  No station UTC offset, so the time shown is the clock's. Logged as
  `weather_fetch` (status and counts, never the station or position).

## Screens (mini, 128×64)

Text is the stock 6×8 font in slots; colours RGB565: VFR `0x07E0`, MVFR
`0x041F`, IFR `0xF800`, LIFR `0xF81F`, unknown `0x8410` (`---`); labels grey
`0x8410`; the age (`1h35m ago`, shown past 90 minutes) orange `0xFD20`.

- **Summary** (`FUN_4200fbec`): row y 4 the local time, then right-aligned to
  column 19 the first of `KBOS ● IFR`, `KBOS IFR`, `KBOS ●`, `IFR` that fits;
  rows y 16/28/40/52 packed with Wind, Vis, Ceil, Temp (grey label padded to
  5 columns when the value leaves room): `350°/23G36kt`, `10+ SM`,
  `7,000 ft`, `52°F`; the age right-aligned on the last row.
- **Simple** (`FUN_4200fe6c`): the 32×32 icon at (4,6); the time centred at
  (40,7,86); the temperature at text size 2 centred at (40,19,86) (else the age,
  else the condition's name); the station name (else ICAO) `● CAT` centred at
  y 49.
- **Technical** (`FUN_4200fffc`): the header as Summary's (with the age when
  old); the raw METAR without its first token when that is the station and
  without `RMK…`, word-wrapped to 21 columns, 4 lines a page, a page every 4 s,
  the last line of a page 3 columns shorter and ending in `▸` when more follow.

Formatters (`FUN_4200ebc0` wind, `FUN_42006d7c` visibility, `FUN_42006f00`
ceiling, `FUN_4200ed10` temperature) and every rounding rule are in
`fw_weather.c`, with the vendor's worked examples in `tests/test_weather.c`.
The display redraws only rows whose text changed, at most every 200 ms.

**Icons:** the vendor's `/images/weather/32/<cond>.png` from the wall's
filesystem when it has them (UPDATES.md "Vendor images"), decoded into the
logo slot no flight needs meanwhile; otherwise the owned drawings
(`fw_weather_icon_draw`: sun, moon, clouds, rain, snow, fog, bolt).

**Glyphs:** `●` (U+25CF, a hand-drawn glyph) and `▸` (U+25B8 → CP437 0x10)
come with the 4.2.4 text tables (`tools/make_vendor_tables.py`).

## Memory

Static RAM: the record twice (the store, the display's copy) and the feed's
parse buffer (3 × 328 bytes), one frame (1,096). The fetch allocates its JSON
documents (2–4 KiB) only while it runs.
