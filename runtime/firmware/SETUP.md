# Setup screens — recovered from the vendor firmware

What the stock firmware shows when it needs Wi-Fi credentials, recovered from
vendor app `0ab195d6…` (whole-program Ghidra decompile). The owned firmware
draws the same pages and runs the setup access point and provisioning
protocol (see "Owned setup mode" below).

## When setup starts — `FUN_42016f08`, `FUN_4201f9a0`

When the stored network fails (Wi-Fi state 6) the display shows `Connection
failed` for 2 s, then `Starting setup mode...` (centred status messages), and
starts a soft access point:

- SSID `Flightwall-` + the device id, the password below, IP `192.168.4.1`,
  netmask `255.255.255.0`.
- On failure: `WiFi` / `Identity error` (no identity) or `WiFi` / `AP start
  failed`, as two white lines (`FUN_4201aa00`).
- A successful join shows `Connected` for 1 s.

## Identity — `FUN_4202353c`, `FUN_4202cf74`, `FUN_4201f2dc`

Created once by the stock firmware and kept in the NVS namespace `identity`:
`id` (6 characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, drawn from the
hardware RNG), `fp` (20 characters), `cert_der` (a self-signed certificate,
CN `flightwall-<id>`, valid 2024–2035), `priv_der`, and `provisioned`.

| Shown | Rule |
| --- | --- |
| SSID | `Flightwall-<id>` |
| Password | the first 8 characters of `fp` (`FUN_42023408`); shorter → identity error |
| QR text | `FW2;ID=<id>;FP=<fp>` (`FUN_42023380`) — for the vendor app, not a Wi-Fi join code |

The owned firmware reads `id` and `fp` with `nvs_open(..., NVS_READONLY)`; it
never writes NVS and never logs these values.

## Pages — `FUN_42021ad8`

Three pages, 4 s each (`0xfa0`, `FUN_4201f70c`), starting at page 0. Every page
is cleared, gets the frame pass, then:

| Page | Content |
| --- | --- |
| 0 | `Scan QR or type` / `credentials to` / `begin setup`, white, x = 3, y = 3 / 15 / 27 (`FUN_4201aafc`). While reconnecting to a known network: `Attempting to` / `reconnect...` in 0xFD20 (`FUN_4201ab68`) |
| 1 | QR code (`FUN_4201ac4c`): ricmoo/QRCode, version 3, low error correction; module scale 2 when size × 2 ≤ 60, else 1; centred (29 × 2 = 58 px at (35,3)); dark modules white, light modules black. If the QR buffer cannot be allocated: `Scan in App:` and the QR text as two lines |
| 2 | `SSID` (3,3), ssid (3,15), `Password` (3,29), password (3,41), white (`FUN_4201abf0`) |

Other messages during setup (`FUN_4201e0a4`): e.g. `Wrong password?` with an
empty second line for 2 s (default `Connection failed`), then the pages resume.

## The QR generator

The vendor image contains ricmoo/QRCode's error-correction tables byte for
byte (codeword table at file offset 0xab06, block table at 0xaa66), and the
call matches its API (`qrcode_getBufferSize(3)`, `qrcode_initText(&qr, buf, 3,
ECC_LOW, text)`, `qrcode_getModule`). The owned firmware builds the same
library, pinned at `eafbde4` (`tools/fetch_deps.py`), behind `fw_qr.h` (its
header defines its own C `bool`).

Checked on the desktop: ricmoo's symbol differs from segno's in data padding
but both are valid — reading the codewords back out of ricmoo's matrix, its
Reed–Solomon codewords match a fresh computation over its own data. zbar
decodes the drawn panel pixels to the exact text.

## Verification

`tests/test_setup.py` (5,093 checks, made-up identities): setup strings, the join QR text, the
identity-error rules, the QR position, size and 2 × 2 modules, the finder
pattern, zbar decodes of both QR pages from the panel pixels, and every text line's rows and
x = 3. Previews: `setup_prompt`, `setup_qr`, `setup_credentials`,
`setup_reconnect`.

## Provisioning protocol — `FUN_4201ec58` and handlers

The setup server is HTTPS (`esp_https_server`) on the access point, using the
identity certificate. **`fp` is the pin of that certificate**: base32 of the
first 96 bits of SHA-256 over the certificate DER (19 characters plus a pad
`A`, `FUN_42023488`; checked against the board's own NVS backup without
printing it). The vendor app reads `fp` from the QR code and trusts only that
certificate.

Every route first checks the request came in on the access point; otherwise
`403 Forbidden` `{"error":"softap_only"}` (`FUN_4201ee50`).

| Route | Behaviour |
| --- | --- |
| `GET /.well-known/flightwall-info` | `{"version":"FW2","id":"<id>","fp":"<fp>"}` |
| `POST /provision` | JSON ≤ 1024 bytes with `ssid` (1–32 bytes), `pass` (< 64 bytes) and `open` (bool; an empty `pass` needs `open:true`). Shows `Connecting...`, joins the network while keeping the access point (Arduino `WiFi.begin`, persistent, auto-reconnect), waits up to 25 s. Joined → `200 {"status":"accepted","restart":true,"restart_in_seconds":5}` and a restart task `fw_prov_restart`. Failures: `400 {"error":"invalid_json"}`, `400 {"error":"invalid_input"}`, `409 {"error":"busy_or_failed"}`, `413 {"error":"too_large"}`, `500 {"error":"oom"}`, `408 {"error":"connection_timeout",...}`, and `400 {"error":"connection_failed","reason":"auth_failed"\|"network_not_found"\|"association_failed"\|"no_response","wifi_reason":N}`; the panel shows `Wrong password?` / `Network not found` / `Failed - Try again` for 2 s |
| `GET /status` | `status` (`online`, `joining`, …), the `<name>.local` host name, `"version":"4.0.3"`, `"model":"mini_v1"`, and the device `api_key` once joined |
| `GET /list_wifi` | `{"networks":[{"ssid","bssid","rssi","channel","encryption","secure"}]}` (`wpa_psk`, `wpa2_psk`, `wpa3_psk`, `wpa2_enterprise`, `wapi_psk`, `open`, `unknown`); `503 {"error":"scan_in_progress"}` / `{"error":"scan_failed"}` |
| `POST /disconnect`, `POST /disconnect_client` | drop the joining attempt / the phone |
| `POST /reset` | `{"status":"ok","action":"reset_and_restart"}`, then a reset task `fw_reset` |

Credentials are saved the way the Arduino core saves them: the Wi-Fi driver's
own NVS storage (`nvs.net80211`), which the owned firmware already reads at
boot. The device is then marked `provisioned` in the `identity` namespace.

## Owned setup mode — trial build

`main/setup_server.c` (enabled with `CONFIG_FW_SETUP_TRIAL`, off by default):

- Scans for networks once, then starts `Flightwall-<id>` (WPA2, the stock
  password) alongside the station at 192.168.4.1, as soon as the station has
  an address (the network probe is skipped in trial builds). Wi-Fi storage
  stays RAM-only. Scanning moves the shared radio off the access point's
  channel and phones drop off, so `/list_wifi` serves that first list and only
  rescans on `?refresh=1` (the page warns before it does).
- Plain HTTP on 80 serves the owned setup page and the stock routes, only to
  clients on the access point. Captive portal: a DNS responder on the access
  point answers every A query with 192.168.4.1 (`main/captive_dns.h`), and any
  unknown path redirects to `/`, so phones open the page by themselves.
- The page is one small self-contained file (≈5 KB), mobile-first: tappable
  network list with signal bars, password with show/hide, plain-language
  results, and a web manifest + icon so it can be added to the home screen. A
  full PWA (service worker, offline) needs a trusted HTTPS origin, which a
  device on 192.168.4.1 cannot have.
- `CONFIG_FW_SETUP_HTTPS` (off by default) adds the stock HTTPS server with
  the identity certificate and the vendor QR page — only useful for setting up
  the owned firmware with the vendor app (≈160 KB). Stock keeps its own setup
  either way, and the owned firmware never changes the identity.
- `/provision` follows the stock validation and error responses, shows
  `Connecting...`, test-joins with up to three tries per failure class and a
  25 s limit, then **restores the previous station config**; success answers
  `{"status":"accepted","restart":false,"restart_in_seconds":0,"saved":false}`
  and the panel shows `Connected` / `Trial: not saved`. Nothing is saved and
  nothing restarts.
- **Saving** (`CONFIG_FW_SETUP_SAVE`): the test join uses the station config
  stock builds (`FUN_42036dc4`, Arduino `WiFi.begin`: all-channel scan,
  strongest AP first, RSSI threshold −127, WPA2 minimum when a password is
  given, PMF capable). After a successful join it is saved exactly as stock
  saves it — the Wi-Fi driver's own NVS storage, switched to FLASH only around
  that one `esp_wifi_set_config` — and, like stock, **only when it differs from
  the stored config** (re-entering the current network writes nothing).
  `identity/provisioned` is set to 1 only if it is not already 1
  (`FUN_42023330`). Then the stock answer
  `{"status":"accepted","restart":true,"restart_in_seconds":5}`, the panel
  `Connected` / `Restarting...`, and a restart after 5 s. The log records the
  outcome (`changed`, errors) but never the values. This is the owned
  firmware's only NVS write; without the option it test-joins only.
- `/reset` → `403 {"error":"not_supported"}` (stock wipes settings);
  `/status` never includes the `api_key`; `/disconnect*` are no-ops.
- Panel: the stock prompt (4 s), the standard `WIFI:T:WPA;S:…;P:…;;` join QR
  held 15 s so a phone has time to scan it, and the stock SSID/password page
  (6 s); with HTTPS on, the stock pages at 4 s each plus the join QR.
- The log records when the access point starts beaconing and on which
  channel, phones joining and leaving (a count, no addresses), the scan
  result count, and each request's path and status; never the SSID, password,
  identity or submitted credentials.

The trial candidate is built with `CONFIG_FW_SETUP_TRIAL=y`,
`CONFIG_FW_VIEW_SECONDS=480` and `CONFIG_FW_PENDING_RESTART_S=540`: about
8 minutes of setup mode before the rollback.

First trial run (2026-10-19, `owned-skeleton-v1-a6dd90b66947`): the access
point and both servers (HTTP and HTTPS) started 34 s after boot (free heap
121 KB with both up); the owner's phone joined `Flightwall-…` by scanning
the join QR and loaded the setup page, but the 3½-minute window closed
before a test was submitted. Clean rollback with state unchanged.

Still to do: saving the credentials the stock way (the driver's own NVS
storage) and restarting, entering setup mode only when the stored network
fails, the `Connected` → restart sequence, a captive-portal DNS answer so
phones open the page by themselves, and the vendor app's behaviour after
provisioning (it expects a restart and the `api_key`).

## On the board — verified 2026-10-19

Board run of `owned-skeleton-v1-67d8415cf156` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged). The board read its own identity read-only (17-character
SSID, 8-character password, 37-character QR text) and drew the three setup
pages and the reconnect variant. **The owner scanned the panel's
white-on-black QR with a phone: it read `FW2;ID=…;FP=…` with the board's own
6-character id and 20-character fp.** The values appear nowhere in the console
log. Panel task stack 2,736 bytes free; three verified route requests ran
alongside (lowest free heap 94,080, again a logo decode during TLS).

## Setup trial on the board — 2026-10-19

Second trial, `owned-skeleton-v1-5b7ede4e23b6` (pending install into app0,
rolled back to `fb336e55529a…` app1 afterwards; firmware status, aliases and
settings unchanged). **The owner joined the setup network from a phone,
opened http://192.168.4.1 and tested the home network: the board joined it
(`joined: true`, no authentication or not-found failures), answered
`/provision` with 200 "accepted, not saved", and went back to its original
network.**

- Access point and both servers up 14.8 s after boot (0.5 s after the station
  got its address); lowest free heap during setup 152 KB.
- Requests: `/` and `/list_wifi` (all 200), `/provision` (200), plain HTTP;
  the HTTPS side was not exercised (no vendor app).
- Problems found: the join QR is up for only 4 s (too short to scan); the
  phone took several minutes to find/join `Flightwall-…`; and while
  `/list_wifi` scanned (200 on the board) the phone dropped off the access
  point and showed "Scan failed" — scanning takes the shared radio off the
  access point's channel.

## Setup trial 3 on the board — 2026-10-19

`owned-skeleton-v1-0b53f52c6e87` (pending install into app0, rolled back to
`fb336e55529a…` app1 afterwards; firmware status, aliases and settings
unchanged). **The setup page opened by itself on the owner's phone and the
test-join of the home network worked.**

- Scan of 13 networks before the access point; access point beaconing on
  channel 6 at 6.4 s, DNS and HTTP up at 6.4 s.
- The phone's connectivity check (`/hotspot-detect.html`, iOS) got the 302
  to the setup page, which loaded within a second of joining; `/list_wifi`
  answered from the cached scan (200) without the phone dropping off.
- `/provision` 200, `joined: true`, no authentication or not-found failures;
  the board went back to its own network afterwards.

## Credential save on the board — 2026-10-19

`owned-skeleton-v1-3682461c0e43` with `CONFIG_FW_SETUP_SAVE`. The owner
joined the setup network (page opened by itself), entered the current home
network and saved:

- Test join `joined: true` (no failures); `/provision` 200 with the stock
  `restart` answer; save `err: 0`, **`changed: true`** — the station config
  stock builds differed from the stored one even for the same network, so the
  driver config **was written to NVS**; `identity/provisioned` already 1, not
  written. Restart 5 s later (`provision_restart`).
- The restart rolled the pending image back to stock (`fb336e55529a…`, app1),
  which **rejoined the home network with the saved config on the same IP**;
  firmware status, aliases and settings unchanged.

Open: which field differed is not known yet (the log deliberately carries no
values). Likely a field the driver normalises when it stores the config (for
example `listen_interval`), in which case stock's own compare would also write
on every setup. Next build logs the names of differing fields (never values).

**After the rollback, stock went into its app-pairing state.** It was
connected to the home network (same IP, reachable), but it had also opened
its own setup network and showed `App connecting...`, because the owner's
phone — which now remembered `Flightwall-…` from these tests — rejoined it.
Stock's main state machine (`FUN_4201cc88`) has a pairing path that starts the
access point while keeping the station (`FUN_42006720` → `FUN_4201fb60`, the
"QR Code" screen) and shows `App connecting...` / `App connected` while a
phone is on it. Once the owner forgot the network on the phone, stock returned
to showing flights by itself.

### What triggers stock's pairing state — `FUN_4201cc88`

Stock's main state machine boots through: 0 join the stored network →
2 network check → 4 → **3: the pairing step** (`FUN_42006720`: start the
setup access point *beside* the station via `FUN_4201fb60(…, 1)`, show the
vendor QR, then the SSID/password page, `FUN_42006804`) → 5 fetch settings
(`Configure settings / in the app to / display flights...` or `Ready!`) →
6 normal running. The access point is then left up: the only routine that
takes it down (`FUN_4201fd34`, `Preparing...`) starts it again a few seconds
later, and only the teardown (`FUN_4201fcf8`) stops it for good. In state 6,
**whenever a phone is associated with that access point** (`+0xa9`, set from
the driver's station count in `FUN_42021e94`), stock moves to state 7 and
shows `App connecting...` / `App connected`; when the phone leaves it goes
back to flights.

So the trigger was the owner's phone, not the saved credentials: it was on
our `Flightwall-<id>` network when the board restarted, and stock's
always-on network has the same name and password, so the phone stayed on.
After the earlier rollbacks the phone did not know the network. To check
without side effects: stock should list `Flightwall-<id>` in a phone's Wi-Fi
list at any time while it shows flights.

Consequences: a phone that remembers `Flightwall-<id>` puts stock into
`App connecting...` whenever it is in range — forget the network after a
setup test. The owned save path is cleared of causing this; the open item is
only why `changed` was true (field names to be logged).

## Saved networks — `FUN_4202d6c4`, `FUN_4202dbec`, `FUN_4202d264`

Stock does **not** boot from the Wi-Fi driver's stored config. It keeps its
own list in NVS namespace `wifi` and connects from that (`FUN_4201f480`):

- Key `nets`: a compact JSON array, newest first, at most 5 entries:
  `[{"ssid":"…","pass":"…","open":false},…]`. Written by ArduinoJson
  (`FUN_420148c0`; escapes only `"` `\` `\b` `\f` `\n` `\r` `\t`, table at
  `PTR_DAT_420008e4 + 4`, NUL as `\u0000`, every other byte raw). Not written
  when longer than 1024 bytes.
- Reading (`FUN_4202d6c4`): `ssid`/`pass` only when they are strings, `open`
  only when it is a boolean; an entry needs a name and either a password or
  `open`. Unusable entries are skipped; at most 5 are kept.
- Legacy keys `ssid`, `pass`, `open` (one network), read only when `nets` is
  absent. With an empty password the entry is used when `open` is missing
  (then treated as open) or nonzero. The owner's NVS backup holds only these
  legacy keys, so this board's stock currently connects from them.
- Adding (`FUN_4202dbec`, called from `FUN_42021c4c` once the station is
  connected with credentials from setup): the new network first, then the
  existing ones with a different name, 5 at most; write `nets`, and after a
  successful write remove the legacy keys. `FUN_4202ddc8` removes a network
  the same way. `FUN_4202c9ec` erases all four keys.
- Choosing (`FUN_4201f120`): with one network, that one; with several, a
  blocking scan (300 ms per channel) and the strongest visible saved network
  (`FUN_4201db4c`), the choice cached for 20 s; none visible → the first.
  Each attempt runs 20 s (`DAT_42000f30`).

So the earlier save, which wrote only the driver config, worked for stock only
because it was the same network. The owned save now also puts the network
first in `nets` exactly as stock does (`components/fw_wifi`,
`main/saved_networks.c`; desktop test `tests/test_nets.c`). Differences, both
leaving NVS in the same final state: the write is skipped when `nets` already
holds exactly the new list and no legacy key is left, and a legacy `open`
stored with a type other than u8 is ignored.

## Wi-Fi failure and reconnect — `FUN_4201cc88`, `FUN_42016b50`, `FUN_42021e94`

Stock has two state variables: the app state (`*DAT_42000e44`: 0 WiFiSetup,
1 WiFiSetupMode, 2 NetworkTest, … 6 FlightDisplay, 9 Reconnecting) and the
Wi-Fi manager's (`+0x50`: 1 connecting, 2 connected, 4 setup, 6 attempt
failed). A reconnect manager (`DAT_42000e4c`) does all retrying in app state 9.

| Situation | Stock behaviour |
| --- | --- |
| Boot, saved networks | splash `Connecting...`, one attempt of 20 s (`DAT_42000f30`) on the network chosen as in "Saved networks" |
| Boot, nothing saved | splash `Starting...`, setup mode at once; `Connected` (green) for 1 s once provisioned |
| Boot attempt connects | splash `Connected` for 1 s → network test |
| Boot attempt fails | splash `Connection failed` (0xFD20) for 2 s → setup mode (access point, pages; page 0 `Attempting to / reconnect...` in 0xFD20) and an immediate retry |
| Retries in setup mode | every 60 s (`DAT_420009b4`) counted from the previous start; each attempt 20 s |
| Phone on the access point | retries pause while it joined ≤ 299 999 ms ago (a phone back within 59 999 ms keeps its join time); the attempt is aborted, page 0 shows the normal prompt; when the pause ends a retry starts at once |
| Lost while online | ≥ 2 s down → app 9; panel `Lost connection, / retrying...` (0xFD20); after > 59 999 ms `WiFi disconnected, / attempting to / reconnect...`; retries at once, then 5 s apart for five attempts, then doubling to 30 s (attempts last 20 s, so the first eight run back to back); after > 299 999 ms lost, setup mode and 60 s retries; never gives up |
| A retry connects | splash `Checking network...`; after > 2 999 ms the network check resolves `api.theflightwall.com` (falls back to 8.8.8.8, 1.1.1.1; rechecks ≥ 2 s apart); passing: panel cleared, `Reconnected`, network test after 4 s |
| Connected, no internet | > 14 999 ms: `Repairing network...` and an IP reset; then every 59 999 ms `Repairing network...` and a full reconnect; > 599 999 ms in app 9: `Restarting...` and a restart, at most once an hour (RTC word 0x50000000 = 0x4E4F4E54 unless uptime > 3 599 999 ms) |

Stock never takes its access point down after a reconnect (only its servers
stop, 4 999 ms after the last phone leaves); its pairing state keeps the
access point up anyway. "Starting setup mode..." is dead code.

**Owned implementation.** `components/fw_wifi/fw_link.c` is the rules above
as a portable, time-driven state machine (desktop test `tests/test_link.c`:
8 176 checks, including the exact retry pattern); `main/wifi_link.c` runs it
every 100 ms on the board: stock's saved list (not the driver config), the
stock station config, Arduino's auto-reconnect within an attempt, the network
check and the restart limit. One deliberate difference: the owned firmware
**stops the access point once it is back online and no phone has been on it
for 4 999 ms**, because it has no app pairing; setup mode exists only while
Wi-Fi is failing. Not modelled yet: the DNS-failure counter of the API client
(more than 4 → reconnect), and what stock shows while its page rotation is
frozen during a phone pause (the owned pages keep rotating).

Test options (`main/Kconfig.projbuild`, all off by default):
`FW_WIFI_FAIL_ATTEMPTS` makes the first N attempts look for a network that
does not exist; `FW_WIFI_DROP_AFTER_S` drops Wi-Fi once, that long after first
getting online, and fails the next `FW_WIFI_DROP_FAILS` attempts.

## Boot splash — `FUN_4201654c`, `FUN_42002788`, `FUN_42002860`

- First use after a reset of its flag: the panel is cleared and
  `/images/system/logo.png` is drawn into the box x 0, y 8, width 128,
  height (int)(2·H·0.9/3.0) = 38 (`FUN_4201af7c` → `FUN_4201b410`, centred,
  not cleared). It is the airline-logo decoder **without the square rule**:
  the image is fitted to the whole box. The 96×64 logo lands as **57×38 at
  (35,8)** (`tests/test_logos.py` checks all 500 logos plus the system logo
  against the independent reference for this box fit).
- Status lines (an array, one String per line): only when the text differs
  from the last drawn, a black strip from y 46 to the bottom, then each line
  centred (x = (W − 6·glyphs)/2 when that is ≥ −1, else 0) from y 49, every
  10 px; empty lines are skipped but still advance. White text is drawn
  transparent (`FUN_4201a7a0`); coloured text (`FUN_4201bd70` →
  `FUN_4201bc04`: `Connection failed` 0xFD20, `Network check failed` 0xFC00)
  is opaque on black. Same text in a new colour is not redrawn.
- The logo flag is reset only by stock's app state 2 (network test) and the
  intro (`FUN_420068e4`, logo at (35,13)); so `Checking network...` during a
  reconnect after a failed boot paints only the strip, over the setup page.
- Owned: `fw_screen_splash` (`components/fw_screens`), `fw_logo_decode_box`
  / `fw_logo_draw_box` (`components/fw_logo`); the logo is decoded once when
  the panel starts.

## Wi-Fi failure run on the board — 2026-10-19

`owned-skeleton-v1-87a1c4bac442` (pending into app0) with
`FW_WIFI_FAIL_ATTEMPTS=2`, `FW_WIFI_DROP_AFTER_S=60`, `FW_WIFI_DROP_FAILS=3`
and saving off. Times from boot:

| t (s) | Event |
| --- | --- |
| 0.9 | stock's saved list read; first attempt (forced to fail); splash `Connecting...` |
| 20.9 | attempt aborted after 20 s; `Connection failed` |
| 22.9–25.9 | setup mode: scan, access point beaconing, servers up; pages with `Attempting to reconnect...` |
| 26.0 / 46.0 | immediate retry (forced to fail), aborted after 20 s |
| 86.0 | next retry 60 s after the previous start, real network |
| 91.3 | connected: `Checking network...` |
| 94.3 | network check passed: `Reconnected`; then clock sync |
| 98.3 | normal screens (after 4 s) |
| 99.5 | access point and servers off (owned: no phone for 5 s) |
| 154.3 | test drop |
| 156.5 | reconnecting after the 2 s debounce: `Lost connection, / retrying...`, attempt 1 |
| 176.5 / 196.5 | attempts 2 and 3, back to back as stock (5 s backoff < 20 s attempts) |
| 216.5 | `WiFi disconnected, / attempting to / reconnect...` (> 59 999 ms lost); attempt 4, real |
| 220.7 / 223.7 / 227.7 | connected, check passed (`Reconnected`), normal screens |
| 480.2 | view end, restart; rolled back to stock |

Every transition matched the recovered timings. No crash; heap low-water mark
138 460 bytes (lowest sampled free 170 920); lowest stack margins: main 1 660, panel 2 480, link 1 728,
DNS 1 504, **system event task 728** — the Wi-Fi and access-point event
handlers run there, so its stack goes from the default 2 304 to 3 584 bytes
(`sdkconfig.defaults`). Stock came back in app1, confirmed, same IP;
firmware status, aliases and settings unchanged; nothing was written.
