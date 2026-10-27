"""Makes the FreeBirdWall icons from the Freebird app's bird.

    py runtime/firmware/tools/make_icon.py [--bird <AppIcon-1024.png, BirdLogo.png or bird.svg>] [--out main]

The bird's outline is traced (marching squares on its alpha mask, then
Ramer-Douglas-Peucker) into one small SVG path, kept in tools/bird.svg so the
icons can be made again without the app's image (the default --bird). The
icons draw the bird as a dot matrix, like the wall's LEDs: a grid of white
dots, each sized by how much of its cell the bird covers. One SVG path per
dot size (zero-length segments with round caps) keeps them to a few KB:

  main/icon.svg   the favicon: the app's blue tile with the dotted white bird,
                  also written as a data: URI into the pages that show it
                  (main/dashboard.html, main/games_page.html and the
                  installer's src/installer.html), and served as /icon.svg
                  by the setup page (setup_server.c)
  main/icon.png   192x192 for phone home screens (apple-touch-icon, the
                  manifests), a 32-colour PNG, served from flash by the
                  dashboard and the setup page
  components/fw_screens/fw_brand.c
                  the bird for the panel's boot splash, BRAND_H rows of
                  4-bit coverage (white, anti-aliased), drawn by
                  fw_screen_brand

Prints the path's size; the trace is checked against the mask (IoU).
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

NL = chr(10)

TRACE_H = 220          # the mask is traced at this height
EPSILON = 0.9          # simplification, in traced pixels
VIEW = 100             # the path's coordinates are 0..VIEW
TOP, BOTTOM = (14, 165, 255), (8, 88, 224)      # the app icon's gradient
BRAND_H = 34           # the splash bird's height in panel pixels (the logo box is 38)
PITCH = 4              # dot grid pitch in path units (25 dots across the bird's box)
DOT_SIZES = (1.3, 1.9, 2.5, 3.0, 3.4)          # dot diameters in path units, by coverage
MIN_COVER = 0.1        # cells the bird covers less than this get no dot
OUTLINE = Path(__file__).resolve().parent / 'bird.svg'


def mask_of(path: Path):
    """The bird as a mask: the alpha of a transparent logo, or the white of
    the app icon (white bird on blue, which keeps the gaps between the wing's
    feathers)."""
    im = Image.open(path)
    if im.mode == 'RGBA':
        m = im.getchannel('A')
    else:
        m = im.convert('RGB').getchannel('R')        # white bird: high red; blue tile: almost none
    m = m.point(lambda v: 255 if v > 128 else 0)
    if im.mode != 'RGBA':                              # drop the tile's corners and edge
        w0, h0 = m.size
        m = m.crop((w0 // 10, h0 // 10, w0 - w0 // 10, h0 - h0 // 10))
    m = m.crop(m.getbbox())
    w = round(m.width * TRACE_H / m.height)
    m = m.resize((w, TRACE_H), Image.LANCZOS)
    return [[m.getpixel((x, y)) > 128 for x in range(w)] for y in range(TRACE_H)], w, TRACE_H


def contours(m, w, h):
    """Marching squares on a padded grid: closed loops of corner points."""
    inside = lambda x, y: 0 <= x < w and 0 <= y < h and m[y][x]
    edges = {}
    for y in range(-1, h):
        for x in range(-1, w):
            # pixel centres at (x+.5, y+.5); cell corners are pixels
            tl, tr, br, bl = inside(x, y), inside(x + 1, y), inside(x + 1, y + 1), inside(x, y + 1)
            idx = tl << 3 | tr << 2 | br << 1 | bl
            if idx in (0, 15):
                continue
            t, r, b, l = (x + 1, y + .5), (x + 1.5, y + 1), (x + 1, y + 1.5), (x + .5, y + 1)
            segs = {1: [(l, b)], 2: [(b, r)], 3: [(l, r)], 4: [(r, t)], 5: [(l, t), (r, b)], 6: [(b, t)],
                    7: [(l, t)], 8: [(t, l)], 9: [(t, b)], 10: [(t, r), (b, l)], 11: [(t, r)], 12: [(r, l)],
                    13: [(r, b)], 14: [(b, l)]}[idx]
            for a, bb in segs:
                edges[a] = bb
    loops = []
    while edges:
        start, nxt = edges.popitem()
        loop = [start]
        while nxt != start and nxt in edges:
            loop.append(nxt)
            nxt = edges.pop(nxt)
        if len(loop) > 8:
            loops.append(loop)
    return loops


def rdp(pts, eps):
    if len(pts) < 3:
        return pts
    (x1, y1), (x2, y2) = pts[0], pts[-1]
    dx, dy = x2 - x1, y2 - y1
    n = (dx * dx + dy * dy) ** .5 or 1
    far, at = -1, 0
    for i in range(1, len(pts) - 1):
        d = abs(dy * pts[i][0] - dx * pts[i][1] + x2 * y1 - y2 * x1) / n
        if d > far:
            far, at = d, i
    if far <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:at + 1], eps)[:-1] + rdp(pts[at:], eps)


def simplify(loop, eps):
    half = len(loop) // 2                       # split a closed loop in two open runs
    a = rdp(loop[:half + 1], eps)
    b = rdp(loop[half:] + [loop[0]], eps)
    return a[:-1] + b[:-1]


def path_of(loops, w, h):
    s = VIEW / max(w, h)
    ox, oy = (VIEW - w * s) / 2, (VIEW - h * s) / 2
    out = []
    for loop in loops:
        pts = [(round(ox + x * s), round(oy + y * s)) for x, y in loop]
        pts = [p for i, p in enumerate(pts) if p != pts[i - 1]]
        if len(pts) < 3:
            continue
        out.append('M' + ' '.join('%d %d' % p for p in pts) + 'Z')
    return ''.join(out)


def render(d, size, tile=True, pad=0.12):
    """The icon at size px: the gradient tile and the bird (even-odd fill)."""
    big = size * 4
    img = Image.new('RGB', (big, big))
    if tile:
        for y in range(big):
            t = y / (big - 1)
            c = tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
            ImageDraw.Draw(img).line([(0, y), (big, y)], fill=c)
    bird = Image.new('L', (big, big), 0)
    inner = big * (1 - 2 * pad)
    k = inner / VIEW
    for part in d.split('Z'):
        if not part:
            continue
        nums = [int(v) for v in part[1:].split()]
        poly = [(big * pad + nums[i] * k, big * pad + nums[i + 1] * k) for i in range(0, len(nums), 2)]
        layer = Image.new('L', (big, big), 0)
        ImageDraw.Draw(layer).polygon(poly, fill=255)
        bird = Image.frombytes('L', bird.size, bytes(a ^ b for a, b in zip(bird.tobytes(), layer.tobytes())))
    img.paste((255, 255, 255), (0, 0), bird)
    return img.resize((size, size), Image.LANCZOS)


def brand(d, out: Path):
    """The bird for the panel: rendered big, cropped, scaled to BRAND_H rows,
    stored as 4-bit coverage, two pixels a byte."""
    big = render(d, 512, tile=False, pad=0).convert('L')
    big = big.crop(big.getbbox())
    w = max(1, round(big.width * BRAND_H / big.height))
    small = big.resize((w, BRAND_H), Image.LANCZOS)
    vals = [small.getpixel((x, y)) * 15 // 255 for y in range(BRAND_H) for x in range(w)]
    if len(vals) % 2:
        vals.append(0)
    data = [vals[i] << 4 | vals[i + 1] for i in range(0, len(vals), 2)]
    rows = (',' + NL).join('    ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) for i in range(0, len(data), 16))
    head = ('/* The Freebird bird for the boot splash, made by tools/make_icon.py from the',
            ' * Freebird app icon; do not edit. %d x %d, 4-bit coverage, two pixels a' % (w, BRAND_H),
            ' * byte (high nibble first), rows top to bottom. */', '#include <stdint.h>', '',
            'const int fw_brand_bird_w = %d, fw_brand_bird_h = %d;' % (w, BRAND_H),
            'const uint8_t fw_brand_bird[%d] = {' % len(data), rows, '};', '')
    out.write_text(NL.join(head), encoding='utf-8', newline=NL)
    print('brand bird %dx%d, %d bytes' % (w, BRAND_H, len(data)))


def dots_of(d):
    """The dot matrix: {diameter: [(x, y) cell centres]} in path units. A dot's
    area follows its cell's coverage, rounded to the nearest of DOT_SIZES."""
    n = VIEW // PITCH
    cell = 16
    mask = render(d, n * cell, tile=False, pad=0).convert('L')
    out = {}
    for j in range(n):
        for i in range(n):
            box = mask.crop((i * cell, j * cell, (i + 1) * cell, (j + 1) * cell))
            cover = ImageStat.Stat(box).mean[0] / 255
            if cover < MIN_COVER:
                continue
            want = DOT_SIZES[-1] * cover ** .5
            size = min(DOT_SIZES, key=lambda s: abs(s - want))
            out.setdefault(size, []).append((i * PITCH + PITCH // 2, j * PITCH + PITCH // 2))
    return out


def dots_path(pts):
    """Zero-length segments, one per dot, relative after the first."""
    out, last = [], None
    for x, y in pts:
        out.append('M%d %dh0' % (x, y) if last is None else 'm%d %dh0' % (x - last[0], y - last[1]))
        last = (x, y)
    return ''.join(out)


def render_dots(dots, size):
    """The icon at size px: the gradient tile and the dotted bird."""
    big = size * 4
    img = Image.new('RGB', (big, big))
    draw = ImageDraw.Draw(img)
    for y in range(big):
        t = y / (big - 1)
        draw.line([(0, y), (big, y)], fill=tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3)))
    k = big * .76 / VIEW
    for dia, pts in dots.items():
        r = dia / 2 * k
        for x, y in pts:
            cx, cy = big * .12 + x * k, big * .12 + y * k
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 255, 255))
    return img.resize((size, size), Image.LANCZOS)


def svg(dots):
    paths = ''.join('<path stroke-width="%g" d="%s"/>' % (dia, dots_path(pts)) for dia, pts in sorted(dots.items()))
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d">'
            '<defs><linearGradient id="g" x2="0" y2="1"><stop stop-color="#0ea5ff"/><stop offset="1" stop-color="#0858e0"/>'
            '</linearGradient></defs><rect width="%d" height="%d" rx="22" fill="url(#g)"/>'
            '<g transform="translate(12 12) scale(.76)" fill="none" stroke="#fff" stroke-linecap="round">%s</g></svg>'
            % (VIEW, VIEW, VIEW, VIEW, paths))


def outline_svg(d):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d"><path fill-rule="evenodd" d="%s"/></svg>'
            % (VIEW, VIEW, d) + NL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bird', type=Path, default=OUTLINE)
    ap.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'main')
    a = ap.parse_args()
    if a.bird.suffix.lower() == '.svg':
        d = re.search(r' d="([^"]+)"', a.bird.read_text(encoding='utf-8')).group(1)
        loops, iou = d.split('Z')[:-1], None
    else:
        m, w, h = mask_of(a.bird)
        loops = [simplify(l, EPSILON) for l in contours(m, w, h)]
        d = path_of(loops, w, h)
        # how close is the trace? intersection over union at the traced size
        check = render(d, max(w, h) * 2, tile=False, pad=0).convert('L').resize((max(w, h), max(w, h)))
        ox, oy = (max(w, h) - w) // 2, (max(w, h) - h) // 2
        inter = union = 0
        for y in range(h):
            for x in range(w):
                p, q = m[y][x], check.getpixel((x + ox, y + oy)) > 128
                inter += p and q
                union += p or q
        iou = inter / union
        OUTLINE.write_text(outline_svg(d), encoding='utf-8', newline='\n')
    dots = dots_of(d)
    (a.out / 'icon.svg').write_text(svg(dots), encoding='utf-8', newline='\n')
    png = render_dots(dots, 192).quantize(32, method=Image.Quantize.MEDIANCUT)
    png.save(a.out / 'icon.png', optimize=True)
    uri = 'data:image/svg+xml,' + (svg(dots).replace('"', "'").replace('#', '%23').replace('<', '%3C')
                                   .replace('>', '%3E'))
    link = '<link rel="icon" href="%s">' % uri
    firmware = Path(__file__).resolve().parent.parent
    for page in (firmware / 'main' / 'dashboard.html', firmware / 'main' / 'games_page.html',
                 firmware.parent.parent / 'installer' / 'src' / 'installer.html'):
        if a.out.resolve() != (firmware / 'main').resolve() or not page.exists():
            continue
        text = page.read_text(encoding='utf-8')
        if re.search(r'<link rel="icon" href="[^"]*">', text):
            text = re.sub(r'<link rel="icon" href="[^"]*">', lambda _: link, text, count=1)
        else:
            text = text.replace('<title>', link + '\n<title>', 1)
        page.write_text(text, encoding='utf-8', newline='\n')
        print('favicon written into', page.name)
    if a.out.resolve() == (firmware / 'main').resolve():
        brand(d, firmware / 'components' / 'fw_screens' / 'fw_brand.c')
    print('loops %d, path %d bytes, %d dots, svg %d bytes, png %d bytes%s' % (
        len(loops), len(d), sum(len(p) for p in dots.values()), len(svg(dots)),
        (a.out / 'icon.png').stat().st_size, '' if iou is None else ', IoU %.3f' % iou))


if __name__ == '__main__':
    main()
