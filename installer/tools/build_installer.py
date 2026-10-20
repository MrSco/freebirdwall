"""Builds the single-file installer page.

    py installer/tools/build_installer.py [--image <unpackaged owned build>] [--out <html>]

One HTML file with everything in it, so a beginner only opens it in Chrome or
Edge (file:// is a secure context, so Web Serial works without a server):

  - src/installer.html, the page;
  - src/rules.js + src/flow.js + src/demo.js + src/app.js as one module
    (installer.html#demo runs it on a pretend wall);
  - esptool-js (vendor/, pinned by SHA-256), loaded from the page through a
    blob URL, and its Apache-2.0 licence in a comment;
  - the owned firmware: the UNPACKAGED keepable build (its descriptor still
    the empty placeholder). The page packages it for each board with that
    board's own key, so this file holds no key and can be handed out.

Checks the image the way tools/package_owned.py does (integrity, one empty
descriptor, ESP32-S3, project freebirdwall) before embedding it.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import re
import shutil
import subprocess
import tempfile
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parent
SRC = ROOT / 'src'
ESPTOOL = ROOT / 'vendor' / 'esptool-js-0.6.1.bundle.js'
ESPTOOL_LICENSE = ROOT / 'vendor' / 'esptool-js-LICENSE'
ESPTOOL_VERSION = '0.6.1'
ESPTOOL_SHA256 = 'ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152'
DEFAULT_IMAGE = REPO / 'runtime' / 'firmware' / 'build-keep' / 'freebirdwall.bin'

sys.path.insert(0, str(REPO / 'runtime' / 'firmware' / 'tools'))
import package_owned  # noqa: E402  the packager's own image rules


def module_text() -> str:
    """The three modules as one: imports between them removed, exports
    turned into plain declarations."""
    out = []
    for name in ('rules.js', 'flow.js', 'demo.js', 'app.js'):
        s = (SRC / name).read_text(encoding='utf-8')
        s = re.sub(r'^import \{[^}]*\} from \'\./[a-z]+\.js\';\n', '', s, flags=re.M)
        s = re.sub(r'^import \{[^}]*\n(?:[^}]*\n)*?\} from \'\./[a-z]+\.js\';\n', '', s, flags=re.M)
        s = re.sub(r'^export \{[^}]*\};\n', '', s, flags=re.M)
        s = re.sub(r'^export (const|function|async function|class) ', r'\1 ', s, flags=re.M)
        if re.search(r'^\s*(import|export)\b', s, flags=re.M):
            raise SystemExit('%s: an import/export was left after joining' % name)
        out.append('// ---- %s\n%s' % (name, s))
    return '\n'.join(out)


def syntax_check(module: str):
    """node --check on the joined module, so a broken page is never built."""
    node = shutil.which('node')
    if not node:
        raise SystemExit('node is needed to check the page script')
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'installer.mjs'
        path.write_text(module, encoding='utf-8')
        r = subprocess.run([node, '--check', str(path)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('the page script does not parse:\n' + r.stderr[-2000:])


def check_image(image: bytes) -> dict:
    try:
        segments, _ = package_owned.verify_integrity(image)
        package_owned.find_placeholder(image, segments)
    except package_owned.PackageError as exc:
        raise SystemExit('firmware image refused: %s' % exc)
    desc = package_owned.app_description(image)
    if desc['project_name'] != 'freebirdwall':
        raise SystemExit('firmware image is %r, not freebirdwall' % desc['project_name'])
    return desc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=DEFAULT_IMAGE)
    ap.add_argument('--out', type=Path, default=ROOT / 'dist' / 'freebirdwall-installer.html')
    a = ap.parse_args()

    bundle = ESPTOOL.read_bytes()
    if hashlib.sha256(bundle).hexdigest() != ESPTOOL_SHA256:
        raise SystemExit('vendor/esptool-js bundle does not match its pinned SHA-256')
    bundle_text = bundle.decode('utf-8')
    if '</script' in bundle_text.lower():
        raise SystemExit('esptool-js bundle contains </script; it cannot be embedded as is')

    image = a.image.read_bytes()
    desc = check_image(image)
    app = module_text()
    if '</script' in app.lower():
        raise SystemExit('the page module contains </script')
    syntax_check(app)

    page = (SRC / 'installer.html').read_text(encoding='utf-8')
    for key, value in (('{{ESPTOOL_VERSION}}', ESPTOOL_VERSION), ('{{ESPTOOL}}', bundle_text),
                       ('{{FIRMWARE}}', base64.b64encode(image).decode('ascii')), ('{{APP}}', app)):
        if page.count(key) != 1:
            raise SystemExit('template placeholder %s missing or repeated' % key)
        page = page.replace(key, value)
    page += '\n<!-- esptool-js %s licence:\n%s\n-->\n' % (ESPTOOL_VERSION, ESPTOOL_LICENSE.read_text(encoding='utf-8').replace('--', '- -'))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(page, encoding='utf-8', newline='\n')
    print('wrote %s (%.1f MB): firmware %s %s, image sha256 %s' % (
        a.out, a.out.stat().st_size / 1048576, desc['version'], desc['compile_date'],
        hashlib.sha256(image).hexdigest()[:12]))


if __name__ == '__main__':
    main()
