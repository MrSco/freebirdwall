"""Fetch the firmware's third-party sources at pinned commits.

    py runtime/firmware/tools/fetch_deps.py

Each goes to runtime/build/deps/<name> (git-ignored). The firmware
build checks each checkout is at its pinned commit and refuses otherwise.
Explicit developer action; the build never downloads anything itself.

  hub75     mrcodetastic/ESP32-HUB75-MatrixPanel-DMA (MIT). Inside the commit
            window matching the vendor build's log strings and config layout
            (see ../PANEL.md); 4de6007 is the newest in that window.
  pngdec    bitbank2/PNGdec 1.1.7 (Apache-2.0), the decoder the vendor uses.
            For the vendor's background (black) its RGB565 output is identical
            to the pre-2025 releases on every logo format present (LOGOS.md).
  littlefs  littlefs-project/littlefs v2.11.3 (BSD-3-Clause). Built with
            LFS_READONLY to read the vendor's logo partition without any
            write code in the image.
  qrcode    ricmoo/QRCode (MIT), the generator the vendor's setup screen uses:
            its error-correction tables are byte-identical in the vendor image
            (SETUP.md). The repository has a single release line; eafbde4 is
            its head.
"""
import subprocess
import sys
from pathlib import Path

DEPS = Path(__file__).resolve().parents[2] / 'build' / 'deps'
PINNED = {
    'hub75': ('https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA.git',
              '4de6007534d9f7585b8518346f86d81573ce232c'),
    'pngdec': ('https://github.com/bitbank2/PNGdec.git',
               '64f9f3bc11cc1acb49f5bce823baae6ece910ba6'),
    'littlefs': ('https://github.com/littlefs-project/littlefs.git',
                 '6cb4e86540eca0d9ba62500a298385c9d863c8be'),
    'qrcode': ('https://github.com/ricmoo/QRCode.git',
               'eafbde494979abc2445c363cc2602230bcbe299c'),
}


def git(*args, cwd=None):
    r = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('git %s failed:\n%s' % (' '.join(args), r.stderr))
    return r.stdout.strip()


def fetch(name, url, commit):
    dest = DEPS / name
    if not dest.exists():
        DEPS.mkdir(parents=True, exist_ok=True)
        git('clone', '--filter=blob:none', '--no-checkout', url, str(dest))
    git('fetch', '--quiet', 'origin', commit, cwd=dest)
    git('checkout', '--quiet', '--detach', commit, cwd=dest)
    if git('rev-parse', 'HEAD', cwd=dest) != commit:
        sys.exit('%s: checkout is not at the pinned commit' % name)
    if git('status', '--porcelain', '--untracked-files=no', cwd=dest):
        sys.exit('%s: pinned checkout has local modifications; refusing' % name)
    print('%-9s %s  %s' % (name, commit[:12], dest))


def main():
    names = sys.argv[1:] or list(PINNED)
    for name in names:
        if name not in PINNED:
            sys.exit('unknown dependency %r (known: %s)' % (name, ', '.join(PINNED)))
        fetch(name, *PINNED[name])


if __name__ == '__main__':
    main()
