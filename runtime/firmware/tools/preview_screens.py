"""Render the panel screens on the desktop, through the exact code the board runs.

    py runtime/firmware/tools/preview_screens.py [--out build/preview]

Writes <name>.png for each screen, drawn as round LEDs on a dark board (8 px
pitch), so layout can be checked before any board run. Needs a desktop gcc and
Pillow.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
PNGDEC = FW.parent / 'build' / 'deps' / 'pngdec' / 'src'
SOURCES = [FW / 'tests/render_screens.c', FW / 'components/fw_screens/fw_screens.c', FW / 'components/fw_screens/fw_brand.c',
           FW / 'components/fw_text/fw_text.c', FW / 'components/fw_text/font5x7.c',
           FW / 'components/fw_text/vendor_charmap.c',
           FW / 'components/fw_clock/fw_clock.c', FW / 'components/fw_clock/clock_tables.c',
           FW / 'components/fw_flight/fw_flight.c', FW / 'components/fw_flight/fw_rotation.c',
           FW / 'components/fw_logo/fw_logo.c', FW / 'components/pngdec/pngdec_unit.c',
           FW / 'components/qrcode/fw_qr.c', FW.parent / 'build/deps/qrcode/src/qrcode.c',
           FW / 'components/fw_sync/fw_sync.c', FW / 'components/fw_games/fw_pong.c',
           FW / 'components/fw_games/fw_breakout.c'] + [
           PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')]
INCLUDES = [FW / 'components/fw_screens/include', FW / 'components/fw_text/include',
            FW / 'components/fw_clock/include', FW / 'components/fw_clock',
            FW / 'components/fw_flight/include', FW / 'components/qrcode/include',
            FW.parent / 'build/deps/qrcode/src',
            FW / 'components/fw_logo/include', PNGDEC,
            FW / 'components/fw_sync/include', FW / 'components/fw_games/include']
DEFINES = ['-D__LINUX__=1', '-Duint=unsigned', '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)']
LOGO = FW / 'build' / 'logo-fixtures' / 'AAL.png'   # private, from tools/extract_logos.py
SYSTEM_LOGO = FW / 'build' / 'system-fixtures' / 'logo.png'   # private, likewise
PITCH = 8


def led_image(ppm):
    from PIL import Image, ImageDraw
    src = Image.open(ppm).convert('RGB')
    w, h = src.size
    out = Image.new('RGB', (w * PITCH, h * PITCH), (18, 18, 20))
    d = ImageDraw.Draw(out)
    px = src.load()
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            c = c if any(c) else (38, 38, 42)      # unlit LEDs are faintly visible on the real panel
            d.ellipse([x * PITCH + 1, y * PITCH + 1, x * PITCH + PITCH - 2, y * PITCH + PITCH - 2], fill=c)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=FW / 'build' / 'preview')
    a = ap.parse_args()
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    a.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'render.exe'
        cmd = [gcc, '-std=gnu11', '-O1'] + DEFINES + ['-I%s' % i for i in INCLUDES]
        r = subprocess.run(cmd + [str(s) for s in SOURCES] + ['-o', str(exe), '-lm'], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        logos = [str(LOGO)] if LOGO.exists() else []
        if logos and SYSTEM_LOGO.exists():
            logos.append(str(SYSTEM_LOGO))
        r = subprocess.run([str(exe), tmp] + logos)
        if r.returncode:
            sys.exit('renderer failed')
        for ppm in sorted(Path(tmp).glob('*.ppm')):
            png = a.out / (ppm.stem + '.png')
            led_image(ppm).save(png)
            print('wrote', png)


if __name__ == '__main__':
    main()
