"""Builds the dashboard's display preview: the wall's own screen code for the browser.

    py runtime/firmware/tools/build_preview.py [--zig <zig.exe>]

Compiles preview/fw_preview.c with the screen, text, clock, flight, logo and
settings sources the firmware uses into main/preview.wasm (committed, so a
firmware build needs no WebAssembly compiler). Needs Zig 0.13 (its clang and
wasi-libc): --zig, $ZIG, `zig` on PATH, or ~/tools/zig-windows-x86_64-0.13.0.
Also needs the fetched sources (tools/fetch_deps.py) and ESP-IDF v4.4.7's cJSON.

Built like the board builds them: char is unsigned (as on the Xtensa
compiler), int, long and pointers 32-bit. tests/test_preview.py checks that
this build and a desktop build of the same sources draw identical frames.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
REPO = FW.parents[1]
DEPS = FW.parent / 'build' / 'deps'
PNGDEC = DEPS / 'pngdec' / 'src'
IDF = Path(os.environ.get('IDF_PATH', Path.home() / 'esp' / 'esp-idf-v4.4.7'))
CJSON = IDF / 'components' / 'json' / 'cJSON'
OUT = FW / 'main' / 'preview.wasm'

SOURCES = [FW / 'preview/fw_preview.c', FW / 'components/fw_screens/fw_screens.c', FW / 'components/fw_screens/fw_brand.c',
           FW / 'components/fw_text/fw_text.c', FW / 'components/fw_text/font5x7.c',
           FW / 'components/fw_text/vendor_charmap.c',
           FW / 'components/fw_clock/fw_clock.c', FW / 'components/fw_clock/clock_tables.c',
           FW / 'components/fw_flight/fw_flight.c', FW / 'components/fw_flight/fw_rotation.c',
           FW / 'components/fw_logo/fw_logo.c', FW / 'components/pngdec/pngdec_unit.c',
           FW / 'components/fw_feed/fw_feed_config.c', REPO / 'shared/owner_alias.c',
           FW / 'components/qrcode/fw_qr.c', DEPS / 'qrcode/src/qrcode.c',
           CJSON / 'cJSON.c'] + [
           PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')]
INCLUDES = [FW / 'components/fw_screens/include', FW / 'components/fw_text/include',
            FW / 'components/fw_clock/include', FW / 'components/fw_clock',
            FW / 'components/fw_flight/include', FW / 'components/fw_logo/include', PNGDEC,
            FW / 'components/fw_feed/include', FW / 'components/fw_sync/include', REPO / 'shared', CJSON,
            FW / 'components/qrcode/include', DEPS / 'qrcode/src']
DEFINES = ['-D__LINUX__=1', '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)']
FLAGS = ['-std=gnu11', '-funsigned-char'] + DEFINES + ['-I%s' % i for i in INCLUDES]
HOST_FLAGS = ['-Duint=unsigned'] if os.name == 'nt' else []   # wasi-libc and glibc have uint; MinGW does not


def find_zig(given=None):
    for z in (given, os.environ.get('ZIG'), shutil.which('zig'),
              Path.home() / 'tools' / 'zig-windows-x86_64-0.13.0' / 'zig.exe'):
        if z and Path(z).exists():
            return str(z)
    return None


def zig_path(given=None):
    return find_zig(given) or sys.exit('Zig 0.13 is needed: pass --zig or set ZIG (https://ziglang.org/download/)')


def check_sources():
    missing = [s for s in SOURCES if not s.exists()]
    if missing:
        sys.exit('missing %s (run tools/fetch_deps.py; ESP-IDF v4.4.7 for cJSON)' % missing[0])


def build_wasm(out=OUT, zig=None):
    check_sources()
    cmd = [zig_path(zig), 'cc', '-target', 'wasm32-wasi', '-mexec-model=reactor', '-Oz', '-flto', '-s',
           '-Wl,--gc-sections', '-o', str(out)] + FLAGS + [str(s) for s in SOURCES]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    return out


def build_host(driver, out):
    """The same sources with the desktop gcc and a driver (tests/test_preview.py)."""
    check_sources()
    gcc = shutil.which('gcc') or sys.exit('a desktop gcc is required')
    cmd = [gcc, '-O1'] + FLAGS + HOST_FLAGS + [str(driver)] + [str(s) for s in SOURCES] + ['-o', str(out), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zig')
    a = ap.parse_args()
    out = build_wasm(zig=a.zig)
    print('wrote %s (%d bytes)' % (out, out.stat().st_size))


if __name__ == '__main__':
    main()
