# Updates and switching firmware

The owned firmware serves the owner's Wi-Fi update service on port 8787 with
the **same keyed protocol as the owner-modified stock firmware** of the
project this one grew from, so the desk (`desk/firmware_device.py`) uploads to
either one the same way (`main/owner_update.c`, `shared/owner_ota.c`).

| Route | What it does |
| --- | --- |
| `GET /v1/firmware` | running slot and image SHA-256 (whole file, as the desk computes it), boot slot, slot size, `update_busy`, `pending_confirmation`; plus `"firmware":"owned"` and `"keepable"` |
| `PUT /v1/firmware` | upload into the other slot: binary body, `If-Match` with the running hash, `X-Firmware-SHA256` |
| `POST /v1/firmware/confirm` | keep a pending image (see below) |
| `POST /v1/firmware/switch` | owned only: boot the image in the other slot |
| `PUT /v1/firmware/spare` | owned only: the vendor firmware, unchanged, into the other slot; what starts does not change (below) |
| `GET /v1/log` | owned only: the recent status lines (`FWSTATE`, `FWMEM`), oldest first, as plain text; the last crash's `FWPANIC` line first, under `--- last crash, in boot N ---` |

Every route needs `Authorization: Bearer <device key>` — the same 32-byte key
the packer writes into the image's `FWOTA1` descriptor, so nothing new is
stored — and is refused to browser pages (any `Origin` or `Sec-Fetch-*`
header). Writes need `If-Match` with the running image's hash.

**Uploads** reuse the stock-side validator itself
(`shared/owner_ota.c`, the same code `tools/package_owned.py`
runs on the desktop): exactly one descriptor with this device's key, the
transfer SHA-256, the ESP image header; then the SDK's own image verification
(`esp_ota_end`) and a flash read-back hash. Only then is the next boot
selected, and the board restarts 0.3 s after answering. A running image that
is not confirmed yet refuses uploads (as stock does).

**Keeping an image.** Test builds (default) refuse `confirm` with 409, so an
installed test image always rolls back to the previous firmware, exactly as
before — the desk reports "unconfirmed". A build made with
`CONFIG_FW_KEEPABLE` accepts the confirmation the desk sends after an upload,
but only when it is healthy (online, panel running); a kept image no longer
restarts at the end of its view window.

**Switching back to stock.** The stock image stays in the other slot, so no
download is needed: `py desk/firmware_cli.py switch` asks the
owned firmware to boot the other slot (refused while the owned image is still
unconfirmed — it returns by itself then). The board checks that the other slot
holds an image that has not failed before, the SDK verifies it while
selecting it, and the board restarts. The owner-modified stock firmware then
starts as a pending image and waits 60 s for a confirmation, which the desk
command sends (`firmware_device.Client.switch_slot`); without it stock
returns to the owned firmware by itself.

**Back to owned** from stock: upload the owned `candidate.bin` as usual.

**The vendor firmware in the spare slot.** A Wi-Fi update goes into the slot
that is not running, so it replaces a vendor firmware kept there (the USB
installer keeps it: it updates in place, installer/src/flow.js `targetSlot`).
`py desk/firmware_cli.py spare <vendor app .bin>` puts it back: the vendor's
application image, unchanged (for example the slot it booted from in the
installer's backup, cut to the image's length), into the slot not running.
The board refuses anything whose app description does not name the vendor's
project (`arduino-lib-builder`) before it writes, then checks the transfer
SHA-256, the SDK's image verification and a read-back, as for an upload. The
boot choice stays: the installer's **Start the vendor firmware again** (USB)
starts it. Not `switch` from the desk: the unmodified vendor firmware has no
Wi-Fi update service, so only USB brings FreeBirdWall back from it.

**Log over Wi-Fi.** Every status line the firmware prints (`FWSTATE`,
`FWMEM`; never secrets) is also kept in a 7 KiB ring in RTC memory
(`main/wall_log.c`), which survives a crash restart (a power cut clears it);
each start adds a `--- boot N, reset reason R ---` line. The desk reads it
with `py desk/firmware_cli.py log`, so a closed-up wall needs no
USB console.

**Board check.** Before any panel pin is touched, every start checks that
this is the board the pin map was recovered for (`main/board_check.c`): the
eFuses say quad flash (not octal) and no octal PSRAM (both would own GPIO
33–37, which the panel drives), flash is 8 MB, the partition table is the
FlightWall's exactly (with the vendor's asset partition), and the vendor's
stored settings name the model `mini-v1` (or none). On a mismatch the panel is
never started and the firmware runs as in safe mode; status says
`"board_ok":false` and the log has the reason. This wall: no embedded flash or
PSRAM (esptool), so the check passes.

**Safe mode.** A pending test image that crashes is rolled back by the
bootloader; a kept image is not, and crashing at every start it would never
reach the network again. So the owned firmware counts crashes in a row (panic
or watchdog resets) in RTC memory (`main/crash_guard.c`). After three it
starts in **safe mode**: Wi-Fi and this update service only — no panel, feed,
dashboard, games or stats — so the desk can still read status
(`"safe_mode":true`), upload a fix or switch to stock. Two minutes of running
without a crash clear the count; any reset that is not a crash (power cycle,
planned restart, an upload) starts it again, so a power cycle retries the
normal start. `CONFIG_FW_SAFE_MODE_TEST` builds a test image that always
starts in safe mode, to check it on the board (it still rolls back).

Not yet: buttons for these in the owned dashboard (they need the key, so they
belong behind the planned authenticated settings API); a stock firmware
without the owner update service needs USB for the first owned install.

## Verification

- Desk: `desk/test_firmware_device.py` adds the switch: normal
  switch with confirmation, refusal while unconfirmed or stale, a bad receipt
  never confirmed, firmware without the route explained (13 tests).
- Board, stage A — 2026-10-19, test build `owned-skeleton-v1-95c5883876c1`
  installed from stock over Wi-Fi with the desk: the update service was up
  1.6 s after boot (running image hashed); the desk's confirmations were all
  refused ("test build"), so it reported "unconfirmed" and the image rolled
  back at the end of its window. Direct checks: status reported the exact
  installed hash in app0, pending, `firmware: owned`, not keepable; no key and
  a wrong key 401; `Origin` or `Sec-Fetch-*` 403; confirm 409; switch 409 (not
  confirmed); a stale `If-Match` 409; an upload while pending 409 with nothing
  written. No crash; stock came back confirmed on the same IP with firmware
  status, aliases and settings unchanged. Heap low-water 45 028 bytes, with the
  start-up network probe (TLS) overlapping the three servers — keepable builds
  leave the probe out (`FW_PROBE`).
- Board, stage B — 2026-10-19, keepable build
  `owned-skeleton-v1-0514eb16a4d8` (no probe) uploaded from stock with the
  desk: installed into app0 in 16 s, **confirmed by the desk** 7.9 s after
  boot (online, panel running) — the first owned image kept on this board.
  Status polls from 20:13 to 20:22 all showed it running, confirmed, in app0.
  One flaw: the main task had already decided at 5.7 s that the image was
  pending, so it still restarted at the end of the 570 s window; the image,
  now valid, simply booted again (no rollback). Fixed afterwards: the window
  ends early and the restart is skipped once the image is confirmed.
  Then `firmware_cli.py switch`: the owned firmware verified and selected
  app1, restarted, and the command confirmed stock — **stock `fb336e55529a`
  running in app1, confirmed**, on the same IP with firmware status, aliases
  and settings unchanged; the owned image stays in app0. Heap low-water mark
  without the probe: 116 800 bytes (was 45 028 with it).
- The stock image's own owner candidate
  (`analysis/route-tls/rebuild-check/owner-provider-v1-fb336e55529a/`)
  passes the desk validator, so from a kept owned image stock can also be put
  back by uploading it, if the other slot ever no longer holds it.
