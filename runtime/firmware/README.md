# Owned firmware

Source-built firmware for the FlightWall mini (ESP32-S3, a 128×64 HUB75 panel),
built with ESP-IDF v4.4.7 against the wall's own bootloader and partition table.
It does what the stock firmware does — the vendor flight feed, the flight,
tracked and clock screens, setup and Wi-Fi rules, the vendor app's settings —
and adds owned features: a dashboard on the wall (port 8080), a community
flight source, Sync trips, tracked flights on community data, stats, games,
extra clocks, airport names, long-name scrolling, Wi-Fi updates and a Wi-Fi
log. For owners, start with the [beginner's guide](../../README.md).

## Documents

| Document | About |
| --- | --- |
| [PANEL.md](PANEL.md) | The HUB75 panel: pins, timing, the driver |
| [FLIGHT-SCREENS.md](FLIGHT-SCREENS.md) | The flight and tracked cards, text rules, long text |
| [FEED.md](FEED.md) | The vendor feed, the community source, tracked flights |
| [SETTINGS.md](SETTINGS.md) | The owned settings, the app's changes, the dashboard API |
| [DASHBOARD.md](DASHBOARD.md) | The dashboard page |
| [SYNC.md](SYNC.md) | Plus Sync as stock does it, and the owned Sync |
| [CLOCK.md](CLOCK.md), [LOGOS.md](LOGOS.md), [GAMES.md](GAMES.md), [ANALYTICS.md](ANALYTICS.md) | Clock, logos, games, stats |
| [SETUP.md](SETUP.md) | Setup mode, Wi-Fi join and reconnect rules |
| [UPDATES.md](UPDATES.md) | Wi-Fi updates, switching firmware, the Wi-Fi log, the board check, safe mode |

## Safety properties

- **NVS is never erased**, and stock's namespaces (`usercfg`, `api`,
  `identity`, `wifi`) are only read. The owned firmware writes its own
  namespace only when the owner changes something (settings, sign-ins, Sync,
  airport names, the FlightWall key, the app-settings fingerprints): no timed
  writes, so flash wears as with stock. The Wi-Fi driver runs with RAM-only
  storage.
- **Board check before the panel** ([UPDATES.md](UPDATES.md)): quad flash, no
  octal PSRAM (both would own the panel's pins), 8 MB, the FlightWall
  partition table, the vendor model `mini-v1`. On a mismatch the panel stays
  off. The USB installer makes the same check before it writes anything.
- **Crashes**: three in a row start safe mode (Wi-Fi and the update service
  only); each crash writes an `FWPANIC` line to the Wi-Fi log.
- **Never prints** credentials, keys, pairing codes or settings values.
- The bootloader and partition table are never written.

## Installing

- **First install, by USB:** the browser installer
  ([`installer/`](../../installer)) checks the board,
  saves a backup, writes the firmware next to the vendor's and makes it start.
- **Updates over Wi-Fi:** a keepable build, packaged with this wall's key
  (below), uploaded by the desk (`desk/firmware_device.py`),
  which confirms it once it is healthy.

```powershell
py tools/fetch_deps.py                      # once: the pinned HUB75, PNGdec, littlefs and QRCode sources
py ../net/fetch_source.py                   # once: the pinned Mbed TLS release
. .\tools\idf-env.ps1                       # ESP-IDF v4.4.7
idf.py -B build-keep -D SDKCONFIG=sdkconfig.keep -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.keep.defaults" build
py ../../installer/tools/build_installer.py # the USB installer page, from build-keep/freebirdwall.bin
py ../../installer/tools/publish_site.py    # put it on the website (docs/)
py tools/package_owned.py --image build-keep/freebirdwall.bin --out build-keep/package   # a Wi-Fi update for one wall
py tools/build_preview.py                   # after changing the screen code: the dashboard preview (needs Zig 0.13)
```

Desktop tests: `py tests/run_host_tests.py` and `tests/test_*.py` (`test_preview.py` needs Node.js;
DASHBOARD.md "Preview").

## Packaging for the Wi-Fi updater

The device's updater ([`owner_ota.c`](../../shared/owner_ota.c))
accepts only an image containing exactly one 44-byte capability descriptor:
`FWOTA1\0\0`, version 1, and this device's 32-byte key. The build leaves an
empty placeholder ([`owner_descriptor.c`](main/owner_descriptor.c)), so the
compiled image carries no secret.

```powershell
py tools/package_owned.py
```

[`package_owned.py`](tools/package_owned.py) reads the key from
`desk/device.json`, writes it into the placeholder, recomputes the
image checksum and SHA-256, then proves the result is installable:

- the desk's own upload validator (`firmware_device.validate_image`) accepts it;
- the **device's own validator code**, compiled for the desktop, accepts the
  upload stream at six chunk sizes (1 byte to whole image) and rejects both a
  wrong key and the unpackaged image.

The one check that cannot run offline is the board's final ESP-IDF image
verification; the desk validator and esptool check the same checksum and SHA
footer independently. Output goes to `build/package/owned-skeleton-v1-<sha>/`
(`candidate.bin` + `release.json`), which is git-ignored. **`candidate.bin`
contains the device key; keep it private.** The key is never printed;
`release.json` records only a short fingerprint of it.

Packer tests (synthetic images, no real key): `py tests/test_package_owned.py`.

## Why ESP-IDF v4.4.7

The installed bootloader matches v4.4.7's `bootloader_dio_80m.elf` byte for
byte. Building the app with the same SDK guarantees that bootloader loads it and
that the Wi-Fi driver reads the stored credentials in the format it wrote them —
without ever replacing the bootloader. v4.4 is past end of life; our TLS is
independent of the SDK's, but lwIP/Wi-Fi fixes would have to come from a later
SDK, whose compatibility with this bootloader would need to be proven first.

## Build (offline)

Requires ESP-IDF v4.4.7 at `~/esp/esp-idf-v4.4.7` with its tools installed for
`esp32s3`, and the pinned Mbed TLS source (`py runtime/net/fetch_source.py`).

```powershell
. $HOME\esp\esp-idf-v4.4.7\export.ps1
cd runtime/firmware
idf.py set-target esp32s3
idf.py build
py tools/firmware_report.py
```

`firmware_report.py` exits 2 if the image does not fit the 2.75 MiB app slot,
its header differs from the installed image (DIO, 80 MHz, 8 MB, ESP32-S3), or
an unprefixed `mbedtls_`/`psa_` symbol escapes the private TLS library. It
writes `analysis/source-firmware/size-report.json` (private, ignored).

## History: the skeleton stage (2026-10-19)

The first owned build only measured what real Wi-Fi and the source-built
TLS engine cost in RAM, as a pending image that always rolled back. Its
measurements are kept here; everything it lacked has since been built.

### Running the probe on the board

1. Connect the controller by USB and start the passive capture **first**:
   `py tools/capture_console.py --port COM5`. It never writes to the port or
   toggles DTR/RTS (either could reset the chip and roll back too early), and it
   reopens the port across each restart.
2. Upload `candidate.bin` through the device dashboard's **Updates** tab or the
   desk, as for any owner update. The board selects it and restarts.
3. The probe runs for about one to two minutes, printing `FWMEM` lines, then
   restarts. The bootloader returns to the previous firmware.
4. The uploader's confirmation step **will fail** — the skeleton has no update
   endpoint — and the dashboard will report the update as unconfirmed. That is
   expected; it is what keeps the rollback.
5. The capture summarises the run. Afterwards, check the dashboard shows the
   previous image hash running and confirmed.

The upload overwrites whatever is in the inactive slot, as every owner update
does; the previous running image stays in its own slot and is what comes back.

### Measured: full link — 2026-10-19

Figures are for the hardened build (task-watchdog panic on, PHY calibration
storage off, owner descriptor slot); RAM is identical to the first link.

ESP-IDF v4.4.7, GCC 8.4 (`esp-2021r2-patch5`), `-Os`. Builds clean; image header
matches the installed firmware (ESP32-S3, DIO, 80 MHz, 8 MB); no unprefixed
`mbedtls_`/`psa_` symbol escapes the private TLS library.

| | Bytes |
| --- | ---: |
| App image | 659,776 (23% of the 2,883,584-byte slot) |
| Static RAM (data + bss) | 91,493 |
| — of which the reserved TLS arena + context | 51,200 |
| — everything else (Wi-Fi, lwIP, TLS tables, RTOS, IDF) | ~40,300 |
| IRAM code (shares the same SRAM) | 67,366 |
| **Main heap region** (`_heap_start`..top of DRAM) | **203,368** |

| Library group | Flash code | Rodata | IRAM | Static RAM |
| --- | ---: | ---: | ---: | ---: |
| Wi-Fi binary blobs | 149,794 | 15,043 | 15,877 | 14,542 |
| lwIP / netif | 83,453 | 16,394 | 0 | 3,974 |
| **Owned TLS** (Mbed TLS + client + transport) | 71,316 | 6,809 | 0 | 8,845 |
| libc / libgcc | 62,277 | 5,927 | 1,534 | 717 |
| Other IDF | 48,368 | 27,430 | 32,289 | 5,937 |
| Wi-Fi driver / supplicant | 43,081 | 3,188 | 534 | 1,977 |
| IDF's own mbedtls (supplicant only) | 25,258 | 3,744 | 74 | 228 |
| FreeRTOS | 373 | 4,597 | 15,643 | 3,877 |
| Skeleton (`main`, incl. arena) | 2,314 | 2,730 | 0 | 51,209 |

What this establishes:

- **The TLS engine fits comfortably**: 71 KB of code against 2.2 MB of free slot,
  where the patched stock image had 1,456 bytes. The blocker from
  [VALIDATION.md](../net/VALIDATION.md) is gone.
- **The TLS memory is provisioned before anything else runs**: the 48 KiB arena
  is in `.bss`, so a handshake never competes with the heap or fragmentation.
- `idf_size`'s "DIRAM remainder" (254,363) overstates free RAM on the S3 because
  IRAM code is carved from the same SRAM; 203,368 is the exact linker figure.
  Startup reclaims some ROM/bootloader memory (adds) and creates its tasks
  (subtracts), so the board's `boot` line will differ somewhat.

Cheap RAM wins now available because we own the build (not applied yet, each
needs its own retest): `MBEDTLS_AES_ROM_TABLES` moves ~8 KiB of AES tables from
RAM to flash; `CONFIG_WPA_MBEDTLS_CRYPTO=n` drops IDF's second mbedtls (flash
only, and WPA3 compatibility must be checked first).

### Measured on hardware — 2026-10-19

One run, installed over Wi-Fi as candidate `a2b40f04ae46…` into app0, captured
passively on USB (private log: `analysis/source-firmware/board-run-20261019-081544/`).
The board booted it as **pending** with the descriptor keyed, ran the probe,
restarted at 39 s, and the bootloader **rolled back** to `fb336e55529a…` in app1.
Firmware status, all four aliases (revision 4) and the settings document were
byte-identical before and after. The uploader reported "unconfirmed", as designed.

| Stage | Free internal heap | Largest block |
| --- | ---: | ---: |
| Boot (arena already reserved) | 297,120 | 253,952 |
| Wi-Fi up with an IP | 254,248 | 245,760 |
| During TLS handshakes | 242,504 | 229,376 |
| **Lowest point of the whole run** | **231,772** | 221,184 (lowest seen) |

- **3/3 route requests succeeded**: HTTP 200, certificate and hostname verified
  (`verify_flags` 0), complete bodies, zero arena blocks left, exactly one close.
- **TLS peak arena payload 25,604 bytes** of the 49,152 reserved; 86 blocks.
- **Probe task stack**: 10,240 allotted, at least 6,220 never touched, so roughly
  4 KiB is used. The main task kept 1,860 of 4,096 free.
- Heap after each request: 242,720 → 241,936 → 242,132. Three rounds cannot
  distinguish a slow leak from Wi-Fi buffer noise; a long run is needed.
- Wi-Fi took 13.8 s to get an address, with one disconnect event before
  success; clock sync 1 s; each request (DNS + TCP + TLS + HTTP) about 4 s.

For comparison, the patched stock firmware ran with roughly 83–102 KiB free and
fell as low as 23,816 bytes, which is why its 96 KiB guard kept deferring
routes. Here the TLS memory is reserved up front and 231 KB was still free at
the worst moment. **That is not like-for-like yet**: this build has no panel
driver, settings, aliases, LAN server or flight feed. The HUB75 DMA buffers are
the next large consumer and must come out of that headroom.

