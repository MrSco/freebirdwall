"""Cross-check fw_logo (C, PNGdec) against an independent Python reference.

    py tests/test_logos.py [--fixtures build/logo-fixtures]

The reference shares no code with the firmware: Pillow decodes the PNGs, and
the RGB565 conversion and scaling are re-implemented from PNGdec 1.1.7's
PNGRGB565 source and the decompiled vendor routines (LOGOS.md). Every logo is
compared pixel for pixel at three square sizes: 32 (mini-v1 DEFAULT), 56
(widescreen LARGE_LOGO) and 64 (no scaling). Fixtures come from
tools/extract_logos.py and stay private.
"""
import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
FW = HERE.parent
PNGDEC = FW.parent / 'build' / 'deps' / 'pngdec' / 'src'
SIDES = (32, 56, 64)
# Whole-box fits (fw_logo_decode_box): the splash logo box on a 64-row panel.
BOXES = ((128, 38),)


# ------------------------------------------------------------ reference

def ihdr(data):
    w, h, depth, ctype = struct.unpack('>IIBB', data[16:26])
    return w, h, depth, ctype


def has_chunk(data, tag):
    pos = 8
    while pos + 8 <= len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        if data[pos + 4:pos + 8] == tag:
            return True
        pos += 12 + n
    return False


def rgb565(r, g, b):
    return ((b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11)) & 0xFFFF


def lines_rgb565(data):
    """PNGdec PNGRGB565(..., LITTLE_ENDIAN, background 0) for each row."""
    w, h, depth, ctype = ihdr(data)
    im = Image.open(__import__('io').BytesIO(data))
    rows = []
    if ctype == 3:                                   # indexed
        idx = np.asarray(im, dtype=np.int32)
        pal = im.getpalette() or []
        pal = pal + [0] * (768 - len(pal))
        trns = im.info.get('transparency')
        alpha = [255] * 256
        if isinstance(trns, (bytes, bytearray)):
            for i, a in enumerate(trns):
                alpha[i] = a
        elif isinstance(trns, int):
            alpha[trns] = 0
        use_alpha = depth == 8 and has_chunk(data, b'tRNS')   # PNGdec blends 8-bit palettes only
        for y in range(h):
            row = []
            for c in idx[y]:
                r, g, b = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
                if not use_alpha:
                    row.append(rgb565(r, g, b))
                    continue
                a = alpha[c]
                if a == 0:
                    row.append(0)
                else:   # a == 255 and the mix both reduce to (p*a)>>shift with a black background
                    row.append(((b * a) >> 11) | (((g * a) >> 10) << 5) | (((r * a) >> 11) << 11))
            rows.append(row)
    elif ctype == 6:                                 # RGBA
        px = np.asarray(im.convert('RGBA'), dtype=np.int32)
        for y in range(h):
            row = []
            for r, g, b, a in px[y]:
                if a == 0:
                    row.append(0)
                elif a == 255:
                    row.append(rgb565(r, g, b))
                else:
                    row.append(rgb565((r * a) >> 8, (g * a) >> 8, (b * a) >> 8))
            rows.append(row)
    elif ctype == 2:                                 # RGB
        px = np.asarray(im.convert('RGB'), dtype=np.int32)
        rows = [[rgb565(r, g, b) for r, g, b in px[y]] for y in range(h)]
    else:
        raise ValueError('pixel type %d not present in the logo set' % ctype)
    return w, h, depth, ctype, rows


def reference(data, side, box_h=None):
    """Vendor fit/scale/sample (FUN_4201b410 + line callback 0x4201b74c), into
    a side x side square or, with box_h, a side x box_h box."""
    bw, bh = side, side if box_h is None else box_h
    w, h, depth, ctype, rows = lines_rgb565(data)
    factor = {2: 3, 4: 2, 6: 4}.get(ctype, 1)
    if w * depth * factor > 3200:
        return None
    f32 = np.float32
    scale = min(f32(bw) / f32(w), f32(bh) / f32(h))
    fw, fh = f32(w), f32(h)
    if scale <= f32(1.0):
        fw, fh = f32(fw * scale), f32(fh * scale)
    dw, dh = max(1, int(np.floor(fw))), max(1, int(np.floor(fh)))
    if box_h is not None and (dw > 64 or dh > 64):
        return 'limit'
    ox, oy = (bw - dw) // 2, (bh - dh) // 2          # C truncation; both non-negative here
    canvas = {}

    def plot(x, y, c):
        if 0 <= x < bw and 0 <= y < bh:
            canvas[(x, y)] = c

    if dw == w and dh == h:
        for y in range(h):
            for x in range(w):
                plot(ox + x, oy + y, rows[y][x])
    else:
        last = -1
        for y in range(h):
            row = oy + (y * dh + h // 2) // h
            if row == last:
                continue
            last = row
            for x in range(dw):
                sx = min(max((x * w + (dw >> 1)) // dw, 0), w - 1)
                plot(ox + x, row, rows[y][sx])
    return dw, dh, canvas


# ------------------------------------------------------------ C side

def build_dump(tmp):
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    exe = Path(tmp) / 'logo_dump.exe'
    zsrc = [PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')]
    cmd = [gcc, '-std=gnu11', '-O1', '-D__LINUX__=1', '-Duint=unsigned',  # MinGW lacks the BSD uint type
           '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)',
           '-I%s' % PNGDEC, '-I%s' % (FW / 'components/fw_logo/include'), '-I%s' % (FW / 'components/fw_text/include'),
           str(HERE / 'logo_dump.c'), str(FW / 'components/fw_logo/fw_logo.c'), str(FW / 'components/pngdec/pngdec_unit.c'),
           *map(str, zsrc), '-o', str(exe), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stderr[-3000:])
    return exe


def read_dump(path):
    data = path.read_bytes()
    pos, out = 0, {}
    while pos < len(data):
        nl, = struct.unpack_from('<H', data, pos); pos += 2
        name = data[pos:pos + nl].decode(); pos += nl
        rc, aw, ah, dw, dh = struct.unpack_from('<iHHHH', data, pos); pos += 12
        canvas = {}
        for y in range(ah):
            for x in range(aw):
                written, c = struct.unpack_from('<BH', data, pos); pos += 3
                if written:
                    canvas[(x, y)] = c
        out[name] = (rc, dw, dh, canvas)
    return out


def check(exe, tmp, files, arg, ref):
    """Runs logo_dump with `arg` (a side or WxH) and compares with ref(data)."""
    dump = Path(tmp) / 'dump.bin'
    listing = Path(tmp) / 'files.txt'
    listing.write_text(''.join(f.as_posix() + '\n' for f in files))
    r = subprocess.run([str(exe), arg, str(dump), str(listing)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('logo_dump failed: ' + r.stderr)
    got = read_dump(dump)
    bad = []
    for f in files:
        exp = ref(f.read_bytes())
        rc, dw, dh, canvas = got[f.name]
        if exp is None or exp == 'limit':
            if rc != -3:
                bad.append((f.name, 'expected a limit refusal, got rc %d' % rc))
            continue
        edw, edh, ecanvas = exp
        if rc != 0:
            bad.append((f.name, 'decode rc %d' % rc))
        elif (dw, dh) != (edw, edh):
            bad.append((f.name, 'size %dx%d vs %dx%d' % (dw, dh, edw, edh)))
        elif canvas != ecanvas:
            diff = sum(1 for k in set(canvas) | set(ecanvas) if canvas.get(k) != ecanvas.get(k))
            bad.append((f.name, '%d pixels differ' % diff))
    sizes = sorted({(got[f.name][1], got[f.name][2]) for f in files if got[f.name][0] == 0})
    return bad, sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fixtures', type=Path, default=FW / 'build' / 'logo-fixtures')
    ap.add_argument('--system', type=Path, default=FW / 'build' / 'system-fixtures',
                    help='folder with the system logo.png (tools/extract_logos.py writes it)')
    a = ap.parse_args()
    files = sorted(a.fixtures.glob('*.png'))
    if not files:
        sys.exit('no fixtures; run tools/extract_logos.py <backup> first')
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_dump(tmp)
        for side in SIDES:
            bad, sizes = check(exe, tmp, files, str(side), lambda d: reference(d, side))
            print('side %2d: %d logos, %d mismatches; drawn sizes %s' % (side, len(files), len(bad), sizes))
            for name, why in bad[:10]:
                print('    %s: %s' % (name, why))
            failures += len(bad)
        boxed = files + sorted(a.system.glob('*.png'))
        for bw, bh in BOXES:
            bad, sizes = check(exe, tmp, boxed, '%dx%d' % (bw, bh), lambda d: reference(d, bw, bh))
            print('box %dx%d: %d images, %d mismatches; drawn sizes %s' % (bw, bh, len(boxed), len(bad), sizes))
            for name, why in bad[:10]:
                print('    %s: %s' % (name, why))
            failures += len(bad)
        logo = a.system / 'logo.png'
        if logo.exists():
            bad, sizes = check(exe, tmp, [logo], '128x38', lambda d: reference(d, 128, 38))
            ok = not bad and sizes == [(57, 38)]
            print('splash logo: %s, drawn %s (stock: 57x38 at (35,8))' % ('ok' if ok else 'MISMATCH', sizes))
            failures += 0 if ok else 1
        else:
            print('splash logo: skipped (no %s)' % logo)
    print('PASS' if not failures else 'FAIL (%d)' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
