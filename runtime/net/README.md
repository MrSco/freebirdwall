# Source-built HTTPS component

Developer prototype for autonomous device networking. This is **not firmware**,
is not linked into the installed owner image, and does not start a PC service.
The board remains on confirmed `fb336e55529a…` firmware.

The component builds Mbed TLS 3.6.7 from a hash-pinned upstream release with
16,384-byte receive and 2,048-byte transmit content buffers. The complete TLS
library is compiled with those sizes. A comparison profile uses 16,384-byte
transmit buffers. TLS 1.2, ECDHE/AES-GCM, required CA/hostname verification and
certificate validity checks are enabled; there is no insecure mode.

TLS allocations stay inside a caller-supplied bounded arena, with no system-heap
fallback. One active connection is permitted at a time. All configured exported
TLS symbols are prefixed `fw_`; a build check rejects unisolated exports. This
keeps its allocator and TLS symbols separate from any SDK library. The SDK-facing
[`tls_port.h`](tls_port.h) exposes only C types and opaque context storage, and
does not propagate this library's headers/configuration to SDK consumers.

The transport must supply a connected socket, bounded send/receive operations,
trusted hardware/system entropy, monotonic time, a yield function and close.
Certificate validity also requires a correct system wall clock. TLS request
deadlines include handshake and application IO; platform DNS/connect work is
separate. Per-handle calls must be serialized. The arena and context must outlive
the request. This is a dedicated TLS engine, not a general shared crypto service.

## Developer build and local acceptance

Requires Python 3.12+, CMake, Ninja and a GCC-compatible C/C++17 compiler. Fetching
is an explicit developer action. CMake verifies the cached source tree and does
not automatically download dependencies. Sources/licenses remain in the ignored
build directory; the [source manifest](mbedtls-source.json) pins the official
archive and SHA256. Upstream is dual licensed Apache-2.0 OR GPL-2.0-or-later;
preserve its license/notice files when distributing linked binaries.

```powershell
py runtime/net/fetch_source.py
cmake -S runtime/net -B runtime/build/net-small -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build/net-small
ctest --test-dir runtime/build/net-small --output-on-failure

cmake -S runtime/net -B runtime/build/net-large -G Ninja -DCMAKE_BUILD_TYPE=Release -DFW_TLS_TX_BYTES=16384
cmake --build runtime/build/net-large
py runtime/net/tests/test_local_https.py --probe runtime/build/net-small/tls_probe.exe --baseline runtime/build/net-large/tls_probe.exe -v
```

On non-Windows hosts omit `.exe`. The local tests use loopback HTTPS with checked-in
**public test-only credentials**. They never access the FlightWall or Internet.
Fixtures cover trusted and rejected certificates, full-size incoming records,
partial IO, reconnects, truncated/unsupported HTTP, memory exhaustion, RNG failure,
timeouts and cleanup. Assertions stay enabled in Release test builds.

The standalone host probe streams bounded HTTP and emits only counts, memory
statistics and a body digest. It never prints aircraft records or credentials.
Explicit public-provider comparisons are available through `compare_live.py
--live --small ... --full ...`; these are not part of automated local tests.

## ESP32-S3 compile check

```powershell
cmake -S runtime/net -B runtime/build/net-xtensa -G Ninja "-DCMAKE_TOOLCHAIN_FILE=C:/path/to/flightwall/runtime/net/xtensa-toolchain.cmake" "-DFW_XTENSA_TOOLCHAIN_DIR=C:/path/to/compiler/bin" -DFW_TLS_HOST_TOOLS=OFF -DBUILD_TESTING=OFF
cmake --build runtime/build/net-xtensa
```

This produces target static libraries, with no device access or firmware image.
It does not establish final firmware link/boot, runtime stack use or panel memory.
The Windows toolchain file currently expects the installed `.exe` compiler names.

## Before device integration

Use the C interface from a separate SDK translation unit; do not mix differently
configured Mbed TLS headers or expose its private allocator to unrelated SDK
crypto users. Implement bounded ESP32 socket/DNS, entropy and clock adapters.
Reserve a measured arena early enough to avoid fragmentation: the current stock
heap does not always have a contiguous 48 KiB block, and exposes no PSRAM.

The ESP32 adapter now exists: [`esp32_transport.c`](esp32_transport.c) supplies
DNS/connect, bounded IO, gated hardware entropy, clock, pause and close. It is
tested on the desktop by `esp_transport_test` and by re-running the loopback
HTTPS suite through it (`tls_local_https_esp_transport`). Its first consumer is
the [owned firmware skeleton](../firmware/README.md).

Combine networking with the existing bounded feed/route decoders and display
path, then measure total RAM (including arena headers, context, parsing, cache,
network/display allocations), stack and watchdog behavior on hardware. Require
failure/reconnect/long-run tests and owner OTA recovery before acceptance. No
stock-library detours from the rejected experiment are reused here.

Measured checkpoint: [VALIDATION.md](VALIDATION.md).
