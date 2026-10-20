# Airline logos — recovered from the vendor firmware

Goal: the owned firmware draws airline logos exactly as the stock firmware does,
using the logo files already on the board. Every rule below comes from the
vendor application `0ab195d6…` (Ghidra 12.1.4 headless; REA rejects Xtensa).

## Assets

The board's data partition labelled `spiffs` (`0x590000`, `0x270000` bytes)
holds **LittleFS**: 496 airline PNGs at `/images/airlines/square/<ICAO>.png`
(492 are 64×64, four are 60×60) plus `heli`, `light`, `smalljet` and `default`.
The vendor mounts it with `esp_littlefs`. Owned firmware reads it with
**littlefs v2.11.3 built `LFS_READONLY`** (no program/erase code in the image;
the block device refuses writes too) and never formats or modifies it.

## Vendor rendering path

| Step | Vendor code | Rule |
| --- | --- | --- |
| Layout | `FUN_42006e34` selects a table by model + `layout_template` | `mini-v1` `DEFAULT`: logo box (2,2,36,36); full catalogue in [`vendor-layouts.json`](vendor-layouts.json) |
| Lookup | `FUN_420083e8` | `al_code` (3 chars, uppercased) → airline PNG if present; else `ac_class` Heli/Light/Smalljet → category PNG; else `default.png` |
| Square | `FUN_42009654` | side = clamp(min(w−4, h−4), 1, 64), centred in the box → 32×32 at (4,4) |
| Decode | `FUN_4201b410` + PNGdec | `openRAM`, `decode(…, 0)` (no fast palette); refuses lines where width × bpp × channels > 3200 |
| Fit | `FUN_4201b410` | float32 scale = min(side/w, side/h), only if ≤ 1; size = `floorf`, ≥ 1; centred in the square |
| Colour | callback `0x4201b74c` | `getLineAsRGB565(pDraw, buf, LITTLE_ENDIAN, 0x000000)`: alpha blended over black |
| Sample | callback `0x4201b74c` | row = (y·dstH + srcH/2)/srcH, first source row wins; col = (x·srcW + dstW/2)/dstW, clamped; clipped to the square |
| Pixel | panel library `drawPixel(565)` | RGB565 → RGB888 by top-bit replication |

Text on the same card is white (constant `0xFFFF`), drawn at the box x and
vertically centred; right alignment applies only to `tag`-style elements.

## Decoder version

The vendor's exact PNGdec version is unknown. It does not matter for these
assets: with a black background, PNGdec 1.1.7's indexed-colour blend is
bit-identical to the pre-2025 premultiply; the RGBA path is unchanged; no logo
is greyscale; the S3 SIMD path is only used without a background colour. We
pin **PNGdec 1.1.7** (Apache-2.0), which also carries 2026 fixes for malformed
palette/transparency chunks. Quirk inherited deliberately: PNGdec ignores
transparency on 1/2/4-bit palettes (17 logos), drawing the palette colour.

PNGdec 1.1.7 declares `PNG_decode`, `PNG_getBpp`, `PNG_getPixelType` and
`PNG_hasAlpha` in its C API but only implements them as C++ methods;
`components/pngdec/pngdec_unit.c` adds C versions with identical bodies.

## Verification

- `tests/test_logos.py`: every logo (500) at squares 32, 56 and 64, firmware C
  (PNGdec) vs an independent Python reference (Pillow decode + PNGdec formulas
  + vendor scaling in numpy float32) — **0 mismatches**.
- `tests/test_assets.py <backup>`: the read-only C littlefs mounts the real
  partition image; all 500 files byte-identical to littlefs-python's
  extraction; image unchanged; resolver checked on the real filesystem.
- Fixtures (`tools/extract_logos.py`) are the vendor's files and stay private
  in the ignored build folder.

## On the board — verified 2026-10-19

Board run of `owned-skeleton-v1-4ac49a8a785b` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged). **The owner confirmed the flight card with the AA logo
looks right.**

| Measurement | Value |
| --- | --- |
| Read-only mount + resolve + read + decode | 538 ms (decode itself ≈5 ms) |
| `AAL.png` | 360 bytes → 32×32, rc 0 |
| Decoder working memory | 44,196 bytes, freed straight after |
| Heap while the decoder was held | 116,668 free (lowest of the run) |
| Route requests with display + logo | 3/3 verified, zero retained, no drift |
| Panel task stack | ~2.2 KB used of 6 KB, including the decode |

The 116,668-byte low point is the decoder overlapping Wi-Fi start-up; earlier
runs bottomed out near 154 KB. In the real firmware, logos are decoded when new
airlines appear, so decoding must be serialised with TLS work and each
airline's 2 KB result cached rather than re-decoded. The decoded-logo buffer
can also shrink from 64×64 to the layout's 32×32.
