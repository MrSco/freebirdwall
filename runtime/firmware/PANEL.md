# Display hardware — recovered from the vendor firmware

The owned firmware must drive the panels exactly as the vendor firmware does:
wrong GPIOs are the one way display work could harm the board. Every value
below was read from the vendor application image `0ab195d6…` (app1 of
`backups/review-2026-10-18-d8618f3b`), not taken from third-party notes.

## Configuration

| Setting | Value | Evidence |
| --- | --- | --- |
| Library | mrcodetastic ESP32-HUB75-MatrixPanel-DMA, S3 `gdma_lcd_parallel16` back end | source paths in DROM `0x3c0f5807`, `0x3c0f7acc` |
| Build profile | PlatformIO env `mini-v1` | same paths; OTA manifest URLs `…/firmware/mini-v1/…` |
| Panel module | 64 × 64 (`mx_width`/`mx_height`) | literal `0x42000cfc` = `0x00400040` |
| Chain | 2 panels → **128 × 64** pixels | stack `chain_length = 2`; GFX base constructed `(0x80, 0x40)` |
| Scan | 1/32 (`ROWS_PER_FRAME` 32, `PIXELS_PER_ROW` 128) | object fields `+0xbe = 0x20`, `+0xbc = 0x80` |
| Driver chip | `shift_driver` 2 = **FM6126A** | stack `driver = 2`; `begin()` runs the shift-driver init when non-zero |
| Line decoder | 0 (default) | stack `line_decoder = 0` |
| Pins (R1 G1 B1 R2 G2 B2) | 2 6 10 3 7 11 | 14-byte table at DROM `0x3c0f5967` |
| Pins (A B C D E) | 39 38 37 36 21 | same table |
| Pins (LAT OE CLK) | 33 35 34 | same table |
| Clock | 8 MHz (`HZ_8M`) | literal `0x42000d00` = 8,000,000 |
| Latch blanking / clock phase | 2 / false | literal `0x42000d04` bytes `[2, 0, 60, 8]` |
| Minimum refresh | 60 Hz | same literal |
| Colour depth | 8 bits per channel | same literal; `begin()` computes `MASK_OFFSET = 16 − depth` |
| Double buffering | off | stack `double_buff = 0` |
| Initial brightness | 128 in the object, then 160 (`0xa0`) during setup | object `+0xb4 = 0x80`; setup calls `(panel, 0xa0)` |

The pin table matches the HD-WF2 X1-connector mapping documented by the board's
community investigation, so binary and external notes agree. On this board,
GPIO 33–37 are free for the panel because the flash is quad (DIO) and there is
no PSRAM; on an octal-flash/PSRAM module they would not be.

## Where it was found

Ghidra 12.1.4 headless on an ELF wrapper of the image (REA's front end rejects
Xtensa targets; scripts in `analysis/ghidra-hub75/scripts`, private):

- `FUN_42019fec` — the library's `MatrixPanel_I2S_DMA::begin()`, inlined from
  the header (uses the "begin() failed!" string; reads the config at `this+0x64`).
- `FUN_4201a46c` — the vendor's panel construction: allocates the 196-byte panel
  object, builds the config on the stack and copies it in.
- `FUN_4201a8a4` — vendor display bring-up: construct, `begin()`, clear, set
  brightness, text settings; draws an error frame if allocation fails.

## Library version

No release tag contains all five of the vendor image's library log strings.
Commits `a15a887`..`4de6007` (2026-05-13..2026-08-29) do, and the vendor's
one-byte `min_refresh_rate` at config offset `0x26` predates `c005c7b`
(2026-09-04), which widened it. Commits in that window differ only by an added
`resumeDMAoutput()` and whitespace. The owned firmware pins **`4de6007`**
(`tools/fetch_deps.py`; the build refuses any other checkout).

## In the owned firmware

`main/display.cpp` uses exactly this configuration, with compile-time checks
that the library's enum values (`FM6126A` = 2, `TYPE138` = 0, `HZ_8M` =
8,000,000) and `HUB75_I2S_CFG` field offsets equal the vendor build's. The
14-byte pin table appears exactly once in both the vendor and owned images.
Built without Arduino/Adafruit GFX (`CONFIG_ESP32_HUB75_USE_GFX=n`).

It starts before Wi-Fi (as the vendor does), draws a diagnostic pattern —
white border, eight colour bars crossing both panel halves and both chained
panels, and an orange square moving along the bottom — at brightness 40/255,
and reports heap before and after `begin()`. A pending image stays up at least
60 s so the panel can be inspected before rollback.

## Verified on the board — 2026-10-19

Two runs, each installed over Wi-Fi as a pending image into app0, captured
passively on USB, then rolled back automatically to `fb336e55529a…` in app1 with
firmware status, all four aliases and settings byte-identical afterwards.

**Picture:** the owner watched the second run (3-minute window) and photographed
it: white border all round, the eight bars in the right colours and order on
both chained panels, the orange marker moving. A dark band in the photo is a
camera artefact — the left border is dark in exactly the same rows, which a
row-addressing fault could not cause, and it is not visible in person.

**Memory** (identical in both runs):

| Stage | Free internal heap | Largest block |
| --- | ---: | ---: |
| Before the panel driver | 296,428 | 253,952 |
| Panel driver running | 223,144 | 180,224 |
| Wi-Fi up with an IP | 176,948 / 177,076 | 163,840 |
| Lowest point of the run (TLS handshakes) | **160,032 / 159,808** | 147,456 (lowest seen) |

- The driver costs **73,284 bytes** (frame buffer, DMA descriptors, object),
  about 7.7 KB more than the 65,536-byte frame-buffer estimate.
- All three route requests succeeded in both runs (HTTP 200, verified, zero
  retained TLS memory) with the panels running.
- The animation task drew 1,500 frames in 60 s (run 1) and 4,505 in 180 s
  (run 2) — its 25 fps target —
  with no stalls during Wi-Fi or TLS, using ~800 bytes of its 3 KB stack.
- Heap stayed at ~176 KB free for the ~145 s the display ran after the network
  work finished; no drift.

For comparison, the patched stock firmware ran with 83–102 KiB free (low of
23,816) with its full feature set. The owned firmware has ~160 KB at its worst
moment with display, Wi-Fi and verified TLS, but still lacks settings, aliases,
the LAN dashboard/update endpoint, fonts and the flight feed.

## Text rendering

The vendor firmware has exactly one font: the classic Adafruit 5x7 table at
`0x3c0f736c`, used with CP437 enabled (no Adafruit `GFXfont` structures exist in
the image). Adafruit-GFX-Library's public `glcdfont.c` (BSD, commit `bc85af5`)
is byte-identical to it, all 1,280 bytes, so `tools/make_font.py` generates
`components/fw_text/font5x7.c` from that source with its licence notice.
The stock display measures accordingly: "Charlotte Douglas" is 101 px wide.

`components/fw_text` (portable C, no SDK dependency): UTF-8 to CP437 so accented
names keep real glyphs (Zürich, Montréal, Málaga); typographic quotes and dashes
normalised; Latin accents CP437 lacks reduced to the base letter (São → Sao,
Łódź → Lódz); malformed UTF-8 renders `?` without reading past the end; width,
centring, scaling 1–4x, clipping, and truncation with a compact one-cell ellipsis
(three single-pixel dots — "..." would cost 18 of 128 px) that drops trailing
spaces. Plus clipped rectangle primitives.

`components/fw_screens` (portable C) holds the screens; the board and the desktop
preview (`tools/preview_screens.py`, LED-style PNGs) run the same code. The test
build rotates a stock-layout flight card (logo placeholder), a text stress screen
and the colour bars every 4 s, with a heartbeat pixel bottom-right.

Tests: `py tests/run_host_tests.py` — 724 checks against hand-written glyph art,
bounds-checked rendering at every position, every Latin-1/Extended-A letter, CP437
round trips, nine malformed-UTF-8 cases and exact truncation widths.

## Original memory estimate

With these settings the library's frame buffer is
32 rows × 8 bit-planes × 128 pixels × 2 bytes = **65,536 bytes of DMA-capable
internal RAM**, plus a few KiB of DMA descriptors. The first owned-firmware run
had at least 221,184 bytes in its largest free block at the worst moment, so it
should fit; the board run must confirm it alongside Wi-Fi and TLS.
