"""Copy the logo PNGs out of a private flash backup for desktop tests/previews.

    py tools/extract_logos.py <full-flash-backup.bin> [--out build/logo-fixtures]

Read-only: mounts the LittleFS partition (0x590000, 0x270000) from an in-memory
copy whose program and erase callbacks raise, so neither the backup nor the
copy can change. The output stays in the git-ignored build folder: these are
the vendor's asset files, not ours to publish.
"""
import argparse
import hashlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FS_OFFSET, FS_SIZE = 0x590000, 0x270000
LOGO_DIR = '/images/airlines/square'


def main():
    from littlefs import LittleFS, UserContext

    class ReadOnly(UserContext):
        def prog(self, *a): raise RuntimeError('program denied')
        def erase(self, *a): raise RuntimeError('erase denied')

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('backup', type=Path)
    ap.add_argument('--out', type=Path, default=HERE.parent / 'build' / 'logo-fixtures')
    a = ap.parse_args()
    raw = a.backup.read_bytes()
    if len(raw) < FS_OFFSET + FS_SIZE:
        sys.exit('not a full 8 MiB flash backup')
    image = raw[FS_OFFSET:FS_OFFSET + FS_SIZE]
    before = hashlib.sha256(image).hexdigest()
    ctx = ReadOnly(buffsize=len(image))
    ctx.buffer[:] = image
    fs = LittleFS(context=ctx, mount=False, block_size=4096, block_count=0,
                  read_size=16, prog_size=16, cache_size=256, lookahead_size=16)
    fs.mount()                      # explicit mount: the default constructor formats on failure
    a.out.mkdir(parents=True, exist_ok=True)
    names = sorted(fs.listdir(LOGO_DIR))
    for name in names:
        (a.out / name).write_bytes(fs.open(LOGO_DIR + '/' + name, 'rb').read())
    system = a.out.parent / 'system-fixtures'          # the boot splash logo, kept apart
    system.mkdir(parents=True, exist_ok=True)
    (system / 'logo.png').write_bytes(fs.open('/images/system/logo.png', 'rb').read())
    fs.unmount()
    if hashlib.sha256(bytes(ctx.buffer)).hexdigest() != before:
        sys.exit('filesystem copy changed during read; refusing')
    print('extracted %d logos to %s (read-only mount, image unchanged)' % (len(names), a.out))


if __name__ == '__main__':
    main()
