"""Run tests/test_assets.c against the real asset partition from a private backup.

    py tests/test_assets.py <full-flash-backup.bin>

Needs build/logo-fixtures from tools/extract_logos.py (littlefs-python), so the
firmware's C littlefs build is checked against an independent implementation.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
LFS = FW.parent / 'build' / 'deps' / 'littlefs'
PNGDEC = FW.parent / 'build' / 'deps' / 'pngdec' / 'src'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    fixtures = FW / 'build' / 'logo-fixtures'
    names = sorted(p.name for p in fixtures.glob('*.png'))
    if not names:
        sys.exit('no fixtures; run tools/extract_logos.py first')
    raw = Path(sys.argv[1]).read_bytes()
    gcc = shutil.which('gcc') or sys.exit('a desktop gcc is required')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'part.img').write_bytes(raw[0x590000:0x590000 + 0x270000])
        (tmp / 'list.txt').write_text(''.join(n + '\n' for n in names))
        exe = tmp / 'test_assets.exe'
        cmd = [gcc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-DLFS_READONLY', '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG',
               '-D__LINUX__=1', '-Duint=unsigned', '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)',
               '-I%s' % (FW / 'components/fw_assets/include'), '-I%s' % LFS,
               '-I%s' % (FW / 'components/fw_logo/include'), '-I%s' % (FW / 'components/fw_text/include'), '-I%s' % PNGDEC,
               str(HERE / 'test_assets.c'), str(FW / 'components/fw_assets/fw_assets.c'),
               str(LFS / 'lfs.c'), str(LFS / 'lfs_util.c'), str(FW / 'components/fw_logo/fw_logo.c'),
               str(FW / 'components/pngdec/pngdec_unit.c')] + [
               str(PNGDEC / n) for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')] + [
               '-o', str(exe), '-lm']
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit('build failed:\n' + r.stderr[-3000:])
        r = subprocess.run([str(exe), str(tmp / 'part.img'), str(fixtures), str(tmp / 'list.txt')], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip())
        sys.exit(r.returncode)


if __name__ == '__main__':
    main()
