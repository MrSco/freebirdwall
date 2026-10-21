# Dashboard polish: firmware memory check

Verified 2026-10-21 with full ESP32-S3 keepable-release builds of base
`6fe4ffd5f080da8ea62c05af1eff5e1d2144d157` and PR code
`363ec75202b01212fce1b0d43c001e2a921de697`.

Both builds used ESP-IDF 4.4.7 and the pinned project dependencies, with
identical generated configuration values from `sdkconfig.defaults` and
`sdkconfig.keep.defaults`. The panel driver was enabled. The toolchain container
was `espressif/idf:v4.4.7`, pinned to digest
`sha256:52bc81e7f212b6cc63b31ea57b8270badb3236e44df8567a57e2d1a6c74c5000`.

| Measurement | Base | PR | Change |
| --- | ---: | ---: | ---: |
| App image | 1,072,992 B | 1,091,792 B | +18,800 B |
| Free space in 2,883,584 B app slot | 1,810,592 B | 1,791,792 B | −18,800 B |
| Static data RAM (data + BSS) | 144,433 B | 144,457 B | +24 B |
| IRAM code, including vectors | 69,198 B | 69,198 B | 0 B |
| Main heap region before startup reclaim/tasks | 148,608 B | 148,584 B | −24 B |
| Panel `animate` function stack frame | 464 B | 688 B | +224 B |

The PR image uses 37.86% of its app slot, leaving 62.14% (1.71 MiB) free.
The panel task retains its existing 6,144 B stack allocation. Its function-frame
increase is stack use inside that allocation, not an additional heap allocation.
Function-frame size does not measure the peak stack use of the entire call chain.

Both `tools/firmware_report.py` runs passed the app-slot fit, stock image-header
compatibility (ESP32-S3, DIO, 80 MHz, 8 MB), and private TLS symbol-isolation
checks. The stack-frame figures come from the Xtensa `entry a1` instructions in
`display.cpp.obj`: `0x1d0` before and `0x2b0` after.

The dashboard HTML grew from 72,923 B to 91,607 B (+18,684 B). The ELF places it
in `.flash.rodata`, starting at `0x3c0ce8fb` in both builds. The existing HTTP
handler sends directly from that embedded flash range. Leaflet, map tiles,
place search, and canvas rendering execute in the browser; the ESP32 does not
load the map library or tiles into its heap.

Build and report commands, run from `runtime/firmware` in the ESP-IDF environment:

```sh
idf.py -B build-memory \
  -D "SDKCONFIG=$PWD/build-memory/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.keep.defaults' \
  reconfigure build
python tools/firmware_report.py --build build-memory \
  --out ../build/memory-verification/pr.json
```

Repeat in the base checkout, writing `baseline.json`. In the verification
workspace, the reports are under `runtime/build/memory-verification/`; ELF,
map, symbol, and disassembly output is retained in each `build-memory/`.
The SDK git-description field was `HEAD-HASH-NOTFOUND` in both non-root container
builds; the actual SDK/toolchain version is fixed by the image digest above.

These are build/linker measurements. Actual free heap, fragmentation, and peak
stack usage while serving requests require the wall's `FWMEM` runtime readings.
No device was accessed and no firmware was flashed during this check.
