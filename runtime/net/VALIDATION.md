# Source TLS validation — 2026-10-19

**Independent component only; no board flash or settings/source changes.**

Mbed TLS 3.6.7 official archive SHA256:
`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`.
Desktop builds used GCC 13.2.0. ESP32-S3 static-library compilation used the
installed Espressif Xtensa 2405 toolchain. Both pass the private-symbol export
check; SDK-facing C code builds without the private TLS configuration leaking
into its translation unit. Target context size is 1,128 bytes (desktop: 1,808).

## Local acceptance

Two native tests plus 13 loopback HTTPS tests pass, including the explicit
full/small-buffer comparison. The reconnect case performs ten trusted requests
with 7-byte socket IO against a 65,536-byte response. Other cases reject wrong
hostnames, untrusted/expired/future certificates, unsupported TLS versions,
truncated bodies, unsupported compression, conflicting/oversized framing,
failed entropy, exhausted arenas, IO failure and deadlines. Fifty mock disconnect
cycles, nested ownership rejection, clock overflow and repeated/reentrant close
cleanup are also covered. All completed cases report zero retained arena blocks
and bytes. Public fixtures are test credentials, not owner/device material.

Controlled loopback comparison (same certificate, response and configuration):

| Transmit content buffer | Peak allocated payload bytes |
| --- | ---: |
| 2,048 bytes | 24,064 |
| 16,384 bytes | 38,400 |
| Reduction | **14,336 bytes (14 KiB)** |

## Live public-provider HTTPS

Explicit desktop probes against the actual position and callsign-route services
both returned HTTP 200 with verified CA/hostname, zero verification flags and
complete bounded HTTP bodies. No owner/vendor credentials were used. Requests
were spaced by at least five seconds. Position probes use public FLL coordinates,
not the owner's configured locations.

| Service | 2 KiB transmit: peak payload | 16 KiB transmit: peak payload | Reduction |
| --- | ---: | ---: | ---: |
| adsb.fi positions | 27,424 bytes | 41,760 bytes | 14,336 bytes |
| ADSBDB routes | 28,792 bytes | 43,128 bytes | 14,336 bytes |

The comparison probes supplied 65,536-byte arenas; a separate verified route
probe also succeeded twice with a 49,152-byte arena. All released every arena
allocation. Live position bodies change between requests; these are transport
measurements, not identical-traffic comparisons or complete route-coverage proof.

`peak_payload_bytes` excludes allocator headers and the separately supplied
context/HTTP buffers, system socket/Wi-Fi memory, parsers, cache and display.
The arena capacity is a real upper bound for this library's arena allocations;
the reported payload peak is **not** total device RAM consumption. Cross-compiling
does not prove equivalent hardware peaks, final stack margin, watchdog behavior
or successful rendered routes. Those remain integration acceptance work.

Private reproducible evidence: `analysis/source-tls/live-comparison.json`, host
and Xtensa build logs, and the target-size object. Board verification afterward
confirmed known-good `fb336e55529a…`, app1, vendor revision 24, original settings,
sleep and aliases, exact dashboard, PC desk stopped and credential exclusion.

## Linked footprint vs. patched stock image — 2026-10-19

Offline only. [`tools/measure_link.py`](tools/measure_link.py) links the full
C interface ([`tools/link_probe.c`](tools/link_probe.c)) against the Xtensa
libraries with `--gc-sections` and compares the result with the registered
`0ab195d6…` base and `owner-provider-v1` blobs (exit status 2 = does not fit).

| Kept by the linker | Code | Rodata | Mutable data/BSS |
| --- | ---: | ---: | ---: |
| Source TLS (Mbed TLS + client) | 66,179 | 11,349 | 8,840 (8.4 KiB AES tables; ~120 B other state) |
| newlib (mostly `printf` family) | 25,403 | 2,516 | 608 |
| libgcc | 2,354 | 0 | 0 |

| Owner space in existing pages | Capacity | Used by provider profile | Free |
| --- | ---: | ---: | ---: |
| Executable (`0x420e14f0`–`0x420f0000`) | 60,176 | 58,720 | **1,456** |
| Rodata (`0x3c12ccdc`–`0x3c130000`) | 13,092 | 8,568 | 4,524 |

**The component cannot be linked into the patched stock image**, even after
trimming libc reuse, AES ROM tables or cipher suites: TLS code alone exceeds the
whole 60 KiB owner executable window. Additional executable pages are not
available. ESP32-S3 instruction and data caches share one MMU table; stock
startup passes the baked `_rodata_reserved_start` literal (`0x3c0f0020`, found
at IRAM `0x40374428`) to `Cache_Set_IDROM_MMU_Size`, giving instruction fetch
entries 0–14 only. Stock rodata occupies entries 15–18, so moving that split
would unmap it. No large dead stock subsystem (BLE/NimBLE, protocomm) exists to
reclaim. Removing the owner provider runtime would not create enough room either.

Consequence: source-built TLS requires source-built firmware (plan step 2), whose
linker owns the IROM/DROM split. The library itself still needs no changes;
`MBEDTLS_AES_ROM_TABLES` and stock-free platform hooks remain cheap RAM trims there.

## Linked into the owned firmware skeleton — 2026-10-19

The same component, linked into the ESP-IDF v4.4.7 skeleton
(`runtime/firmware`, GCC 8.4, `-Os`), keeps 71,316 B code, 6,809 B
rodata and 8,845 B RAM, consistent with the GCC 13 measurement above. The image
is 661,056 B of a 2,883,584 B slot, so the fit problem does not exist in a
source build. The symbol-isolation check passes on the firmware's archive.
On the board (2026-10-19, one run of three requests): all succeeded with
certificate verification, peak arena payload 25,604 B, zero retained blocks,
lowest free heap 231,772 B and roughly 4 KiB of probe stack. Details in
`runtime/firmware/README.md`.
