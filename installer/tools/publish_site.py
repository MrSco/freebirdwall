"""Puts the built installer on the website (GitHub Pages serves docs/).

    py installer/tools/build_installer.py
    py installer/tools/publish_site.py

Copies installer/dist/freebirdwall-installer.html to docs/index.html (the
page at https://mrsco.github.io/freebirdwall/) and docs/freebirdwall-installer.html
(the same page, to save and use offline). The page carries the unpackaged
firmware only; each wall gets its own key in the browser. If the desk's
private connection file (desk/device.json, git-ignored) is present, its key
must not appear in the page.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGE = ROOT / 'installer' / 'dist' / 'freebirdwall-installer.html'


def main():
    if not PAGE.exists():
        sys.exit('build the installer first: py installer/tools/build_installer.py')
    page = PAGE.read_bytes()
    device = ROOT / 'desk' / 'device.json'
    if device.exists():
        token = json.loads(device.read_text(encoding='utf-8')).get('token', '')
        if token and (token.encode() in page or token.upper().encode() in page):
            sys.exit('the page contains this wall\'s key; not publishing')
    for name in ('index.html', 'freebirdwall-installer.html'):
        (ROOT / 'docs' / name).write_bytes(page)
    print('docs/index.html and docs/freebirdwall-installer.html updated (%.1f MB)' % (len(page) / 1048576))


if __name__ == '__main__':
    main()
