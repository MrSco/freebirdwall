# Clock screen — recovered from the vendor firmware

Goal: the owned firmware's clock looks and behaves exactly like the stock
`CLOCK` loading template. Every rule below comes from the vendor application
`0ab195d6…` (Ghidra 12.1.4 headless plus objdump of the IROM segment); the
tables are pulled out of that image by
[`tools/make_vendor_tables.py`](tools/make_vendor_tables.py), which refuses
any other build.

## Settings (stock JSON → `FwClockConfig`)

| Stock key | Vendor parse | Meaning |
| --- | --- | --- |
| `time_zone.standard_utc_offset_minutes` | `FUN_4201c5dc` | minutes; missing = "no zone" (`0x8000`): UTC, no DST (the owned firmware also reads the older top-level `clock_utc_offset_minutes` when `time_zone` has none) |
| `time_zone.dst_rule` | `FUN_4202526c` | `none` 0, `north_america` 1, `europe` 2 |
| `clock.style` | `FUN_420189b4` | `DEFAULT` or `LARGE` |
| `clock.show_date` | `FUN_420189b4` | date line on/off |
| `clock.additional_timezones` | `FUN_420189b4` | at most 2 × `{label, time_zone}`; non-object elements skipped; a missing `time_zone` or offset = no zone (`UTC`), unknown `dst_rule` = none (`FUN_420182bc`) |
| `clock_is_24hr`, `clock_show_seconds` | `FUN_420189b4` | |
| `date_format` `{order, style, show_weekday}` | `FUN_420185dc` | `order` `day_month`/`month_day`; only `abbreviated_month` style exists; invalid object → month first, weekday on |
| `locale` | `FUN_42007664` | 19 languages; unknown → English |

## Local time — `FUN_42025328`

Standard local time is UTC + offset. With a DST rule, the year of standard
local time picks the window (`FUN_42025124` weekday, `FUN_42025178` epoch):

- **north_america**: from 02:00 local standard time on the 2nd Sunday of March
  to 02:00 local daylight time (01:00 standard) on the 1st Sunday of November.
- **europe**: from 01:00 UTC on the last Sunday of March to 01:00 UTC on the
  last Sunday of October.

Inside the window the offset gains 60 minutes. Stock `time_t` is 32-bit.

## Text

| Line | Vendor | Format |
| --- | --- | --- |
| Time | `FUN_420075b4` | 24 h: `%02d:%02d[:%02d]`; 12 h: `%d:%02d[:%02d] %s`, hour 0 → 12, `AM` when hour ≤ 11 |
| Date | `FUN_42007664` | `"%d %s"` (day_month) or `"%s %d"`, then `weekday + " " + that`; month/weekday names upper-case from the locale table; over 31 bytes → empty |
| Zone name | `FUN_42019f90`, `FUN_42019f20` | first table match on *standard* offset (HST AKT PST MST CST EST AST BRT AZT GMT CET EET MSK GST IST ICT CST JST AET NZT), else `UTC±H` / `UTC±H:MM`; no zone → `UTC`. **Owned:** while daylight saving is on, the daylight name for the offset and rule (HDT AKDT PDT MDT CDT EDT ADT; BST CEST EEST), else the offset as it is then (`UTC-2:30`) |
| Extra zone label | `FUN_42013874` | trimmed, ASCII upper-cased; empty → zone name |

## Lines and layout — `FUN_42013874`, `FUN_42013b88`

- Up to 3 lines (2 in `LARGE`): **date** (if shown), **time**, then extra zones
  in the remaining lines. With extra zones the main time line is prefixed with
  the main zone's name: `EDT 10:25 AM` in summer, `EST 10:25 AM` in winter
  (stock names the standard offset all year, `EST` in July).
- Width of a line = code points × 6 × scale. `LARGE` uses scale 2 when the
  longest line × 12 ≤ panel width and lines × 16 + gaps ≤ panel height, else 1.
- The block is centred: `x0 = (W − widest) / 2`, `y0 = (H − (8·scale·n + n − 1)) / 2`
  (C division; 0 if the block overflows by more than a pixel). Each line is
  centred within the widest; lines are 8·scale tall with a 1 px gap.
- All text is white (`0xFFFF`) on black, drawn opaquely (whole 6×8 cells).
- Date lines go through `FUN_4200f7ec`: 42 Greek/Cyrillic/Turkish capitals
  come from the clock's own 5×8 table, everything else through normal text.

The owner's settings (US Eastern, `LARGE`, date day-first with weekday,
12-hour) give two 2× lines: `MON 19 OCT` at (4,15) and `10:25 AM` at (16,32).

## Refresh

The display loop calls the clock renderer on every pass (`FUN_420164c0` →
`FUN_42013b88`). It clears the panel only when the number of lines, the scale
or any line's position/size changed; otherwise it redraws only lines whose text
changed. Entering the template (`FUN_42013fe0`) clears once. Other loading
templates found there: `ELLIPSIS` and `BLANK`.

## Stock text mapping (applies to all text)

Found while recovering the date line: stock text (`FUN_4201a5c8`) maps code
points below 0x80 straight to the font, others through a 134-entry map into
the CP437 font (`FUN_4201bae8`), then 63 extra hand-drawn glyphs
(`FUN_4201baac`: Ã Õ Ø Ł Ś Ž Č Ř Œ € ★ ✈ …), else `?`; its UTF-8 decoder
(`FUN_420d9378`) has no overlong checks. `fw_text` now uses those tables and
that decoder exactly, replacing its earlier base-letter fallback. Punctuation
folding does exist in stock, but earlier, on feed text (`FUN_42008680`, see
[FLIGHT-SCREENS.md](FLIGHT-SCREENS.md)), not in the draw routine.

## Verification

- `tests/test_clock.py`: fw_clock vs the IANA tz database (tzdata 2025.2) for
  19 real zones that follow the stock rules — every hour of the transition
  weeks ±1 s, plus random moments, through 2037: **2,081,504 moments, 0
  mismatches**.
- `tests/test_clock.c` (138 checks): every format, zone name, the layout
  geometry above, extra-zone line limits, glyph drawing, and the redraw rules
  (no clear on an unchanged layout, untouched lines not redrawn, opaque
  redraw identical to a fresh render).
- `tests/test_text.c` (576 checks): the stock tables and decoder, including
  exactly 134 + 63 mapped code points and stock's malformed-UTF-8 results.
- `tools/preview_screens.py` renders `clock`, `clock_seconds`, `clock_zones`,
  `clock_ru`.

## On the board — verified 2026-10-19

Board run of `owned-skeleton-v1-7540f27e68b5` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged). **The owner photographed the clock: `MON 19 OCT` over
`11:19 AM`, both 2×, white, time centred under the date.**

| Measurement | Value |
| --- | --- |
| First clock frame (logged) | `MON 19 OCT` at (4,15), `11:18 AM` at (16,32), scale 2 — the desktop geometry |
| Time source | SNTP synced 3.8 s after boot; first clock frame 1.0 s later |
| Panel task stack | 3,744 bytes free of 6 KB with the clock running |
| Route requests alongside | 3/3 verified, nothing retained; free heap after each 146,808 / 147,072 / 146,804 (no trend) |
| Lowest free heap | 114,852 (logo decoder overlapping Wi-Fi start-up, as in the logo run) |
