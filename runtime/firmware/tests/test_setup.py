"""Check the setup screens (SETUP.md) on the desktop.

    py runtime/firmware/tests/test_setup.py

- The QR code drawn on page 1 must decode, read back from the panel pixels
  by an independent decoder (zbar), to exactly the setup text; it must be a
  29x29 (version 3) symbol drawn at 2x and centred, white on black. (Its
  padding differs from other encoders such as segno; both are valid, and
  ricmoo/QRCode is what the vendor uses. SETUP.md records the codeword check.)
- Setup strings, text positions and the reconnect variant.

Needs pyzbar (bundles zbar on Windows) and Pillow:
  py -m pip install --target runtime/build/deps/py-zbar pyzbar==0.1.9
Identities here are made up; the board's real id/fp are never used in tests.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
DEPS = FW.parent / 'build' / 'deps'
sys.path.insert(0, str(DEPS / 'py-zbar'))
from PIL import Image  # noqa: E402
from pyzbar.pyzbar import decode  # noqa: E402

UINT = ['-Duint=unsigned'] if os.name == 'nt' else []   # PNGdec uses uint: glibc declares it, MinGW does not

PNGDEC = DEPS / 'pngdec' / 'src'
SOURCES = [HERE / 'setup_dump.c', FW / 'components/fw_screens/fw_screens.c', FW / 'components/fw_screens/fw_brand.c',
           FW / 'components/fw_flight/fw_flight.c', FW / 'components/fw_flight/fw_rotation.c',
           FW / 'components/fw_text/fw_text.c', FW / 'components/fw_text/font5x7.c',
           FW / 'components/fw_text/vendor_charmap.c', FW / 'components/fw_logo/fw_logo.c',
           FW / 'components/pngdec/pngdec_unit.c', FW / 'components/qrcode/fw_qr.c',
           DEPS / 'qrcode' / 'src' / 'qrcode.c'] + [
           PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')]
INCLUDES = [FW / 'components/fw_screens/include', FW / 'components/fw_flight/include',
            FW / 'components/fw_text/include', FW / 'components/fw_logo/include', PNGDEC, FW / 'components/qrcode/include', DEPS / 'qrcode' / 'src']
DEFINES = ['-D__LINUX__=1', *UINT, '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)']

CASES = [('K7QM2P', 'ABCDEFGHIJKLMNOPQRST'), ('ZZ23AB', 'MFRGGZDFMZTWQ2LKNNWG'), ('A2B3C4', 'QRSTUVWXYZ234567ABCD')]
checks = 0


def check(cond, what):
    global checks
    if not cond:
        sys.exit('FAIL: ' + what)
    checks += 1


def run(exe, ident, fp):
    out = subprocess.run([str(exe), ident, fp], capture_output=True, text=True, check=True).stdout.split('\n')
    info = dict(line.split(' ', 1) for line in out[:5])
    pages, i = {}, 5
    while i < len(out) and out[i].startswith('page'):
        pages[int(out[i].split()[1])] = out[i + 1:i + 65]
        i += 65
    return info, pages


def main():
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'setup_dump.exe'
        r = subprocess.run([gcc, '-std=gnu11', '-O1', '-w'] + DEFINES + ['-I%s' % i for i in INCLUDES] +
                           [str(s) for s in SOURCES] + ['-o', str(exe), '-lm'], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        for ident, fp in CASES:
            info, pages = run(exe, ident, fp)
            check(info['ok'] == '1', 'identity accepted')
            check(info['ssid'] == 'Flightwall-' + ident, 'ssid')
            check(info['password'] == fp[:8], 'password = first 8 of fp')
            check(info['qr'] == 'FW2;ID=%s;FP=%s' % (ident, fp), 'qr text')
            check(info['join'] == 'WIFI:T:WPA;S:Flightwall-%s;P:%s;;' % (ident, fp[:8]), 'join qr text')
            for which, want in ((1, info['qr']), (4, info['join'])):
                qr = pages[which]
                lit = [(x, y) for y in range(64) for x in range(128) if qr[y][x] == '#']
                x0, y0 = min(x for x, _ in lit), min(y for _, y in lit)
                x1, y1 = max(x for x, _ in lit), max(y for _, y in lit)
                check((x0, y0, x1, y1) == (35, 3, 92, 60), 'QR spans 58x58 at (35,3): %r' % ((x0, y0, x1, y1),))
                for y in range(64):          # every module is a solid 2x2 block
                    for x in range(35, 93, 2):
                        if 3 <= y < 61 and (y - 3) % 2 == 0:
                            block = {qr[y][x], qr[y][x + 1], qr[y + 1][x], qr[y + 1][x + 1]}
                            check(len(block) == 1, 'module at %d,%d is one colour' % (x, y))
                modules = [[qr[3 + 2 * my][35 + 2 * mx] == '#' for mx in range(29)] for my in range(29)]
                check(all(modules[r][c] == (r in (0, 6) or c in (0, 6) or (2 <= r <= 4 and 2 <= c <= 4))
                          for r in range(7) for c in range(7)), 'top-left finder pattern')
                img = Image.new('L', (37 * 8, 37 * 8), 255)
                px = img.load()
                for my in range(29):
                    for mx in range(29):
                        if modules[my][mx]:
                            for dy in range(8):
                                for dx in range(8):
                                    px[(mx + 4) * 8 + dx, (my + 4) * 8 + dy] = 0
                got = [d.data.decode() for d in decode(img)]
                check(got == [want], 'zbar reads %r, want %r' % (got, want))
            # text pages: lit rows only where the stock lines sit
            def rows(page):
                return {y for y in range(64) if '#' in pages[page][y]}
            check(rows(0) <= set(range(3, 11)) | set(range(15, 23)) | set(range(27, 35)), 'prompt line rows')
            check(min(rows(0)) >= 3 and min(x for y in rows(0) for x in range(128) if pages[0][y][x] == '#') == 3,
                  'prompt at x=3')
            check(rows(2) <= set(range(3, 11)) | set(range(15, 23)) | set(range(29, 37)) | set(range(41, 49)),
                  'credential line rows')
            check(rows(3) <= set(range(3, 11)) | set(range(15, 23)), 'reconnect lines')
        info, _ = run(exe, '', 'ABCDEFGH')
        check(info['ok'] == '0', 'no id: identity error')
        info, _ = run(exe, 'K7QM2P', 'SHORT')
        check(info['ok'] == '0', 'short fp: identity error')
    print('PASS: %d setup checks' % checks)


if __name__ == '__main__':
    main()
