"""Compile and run the firmware's desktop C tests with the host gcc.

    py runtime/firmware/tests/run_host_tests.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
PNGDEC = FIRMWARE.parent / 'build' / 'deps' / 'pngdec' / 'src'
PNGDEC_SOURCES = [PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c',
                                       'inftrees.c', 'zutil.c')]
PNGDEC_DEFINES = ['-D__LINUX__=1', '-Duint=unsigned', '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)']
# cJSON as ESP-IDF ships it (components/json), for the saved-network list.
CJSON = Path(os.environ.get('IDF_PATH') or Path.home() / 'esp' / 'esp-idf-v4.4.7') / 'components' / 'json' / 'cJSON'
TESTS = {
    'test_airports': {
        'sources': [HERE / 'test_airports.c',
                    FIRMWARE / 'components/fw_airports/fw_airports.c',
                    FIRMWARE / 'components/fw_airports/airports_table.c'],
        'includes': [FIRMWARE / 'components/fw_airports/include'],
    },
    'test_text': {
        'sources': [HERE / 'test_text.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'includes': [FIRMWARE / 'components/fw_text/include'],
    },
    'test_clock': {
        'sources': [HERE / 'test_clock.c',
                    FIRMWARE / 'components/fw_clock/fw_clock.c',
                    FIRMWARE / 'components/fw_clock/clock_tables.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'includes': [FIRMWARE / 'components/fw_clock/include', FIRMWARE / 'components/fw_clock',
                     FIRMWARE / 'components/fw_text/include'],
    },
    'test_flight': {
        'sources': [HERE / 'test_flight.c',
                    FIRMWARE / 'components/fw_flight/fw_flight.c',
                    FIRMWARE / 'components/fw_flight/fw_rotation.c',
                    FIRMWARE / 'components/fw_screens/fw_screens.c', FIRMWARE / 'components/fw_screens/fw_brand.c',
                    FIRMWARE / 'components/fw_logo/fw_logo.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'third_party': PNGDEC_SOURCES + [FIRMWARE / 'components/pngdec/pngdec_unit.c',
                                         FIRMWARE.parent / 'build/deps/qrcode/src/qrcode.c',
                                         FIRMWARE / 'components/qrcode/fw_qr.c'],
        'includes': [FIRMWARE / 'components/fw_flight/include', FIRMWARE / 'components/fw_screens/include',
                     FIRMWARE / 'components/fw_logo/include', FIRMWARE / 'components/fw_text/include', PNGDEC,
                     FIRMWARE / 'components/qrcode/include', FIRMWARE.parent / 'build/deps/qrcode/src'],
        'defines': PNGDEC_DEFINES,
        'libs': ['-lm'],
    },
    'test_sync': {
        'sources': [HERE / 'test_sync.c',
                    FIRMWARE / 'components/fw_sync/fw_sync.c',
                    FIRMWARE / 'components/fw_flight/fw_flight.c',
                    FIRMWARE / 'components/fw_flight/fw_rotation.c',
                    FIRMWARE / 'components/fw_screens/fw_screens.c', FIRMWARE / 'components/fw_screens/fw_brand.c',
                    FIRMWARE / 'components/fw_logo/fw_logo.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'third_party': PNGDEC_SOURCES + [FIRMWARE / 'components/pngdec/pngdec_unit.c',
                                         FIRMWARE.parent / 'build/deps/qrcode/src/qrcode.c',
                                         FIRMWARE / 'components/qrcode/fw_qr.c'],
        'includes': [FIRMWARE / 'components/fw_sync/include', FIRMWARE / 'components/fw_flight/include',
                     FIRMWARE / 'components/fw_screens/include', FIRMWARE / 'components/fw_logo/include',
                     FIRMWARE / 'components/fw_text/include', PNGDEC, FIRMWARE / 'components/qrcode/include',
                     FIRMWARE.parent / 'build/deps/qrcode/src'],
        'defines': PNGDEC_DEFINES,
        'libs': ['-lm'],
    },
    'test_analytics': {
        'sources': [HERE / 'test_analytics.c', FIRMWARE / 'components/fw_analytics/fw_analytics.c'],
        'includes': [FIRMWARE / 'components/fw_analytics/include'],
    },
    'test_games': {
        'sources': [HERE / 'test_games.c',
                    FIRMWARE / 'components/fw_games/fw_pong.c',
                    FIRMWARE / 'components/fw_games/fw_breakout.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'includes': [FIRMWARE / 'components/fw_games/include', FIRMWARE / 'components/fw_games',
                     FIRMWARE / 'components/fw_text/include'],
    },
    'test_arcade': {
        'sources': [HERE / 'test_arcade.c',
                    FIRMWARE / 'components/fw_games/fw_arcade.c',
                    FIRMWARE / 'components/fw_games/fw_pong.c',
                    FIRMWARE / 'components/fw_games/fw_breakout.c',
                    FIRMWARE / 'components/fw_screens/fw_canvas.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'includes': [FIRMWARE / 'components/fw_games/include', FIRMWARE / 'components/fw_screens/include',
                     FIRMWARE / 'components/fw_text/include'],
    },
    'test_pair': {
        'sources': [HERE / 'test_pair.c', FIRMWARE / 'components/fw_settings/fw_pair.c',
                    FIRMWARE.parent.parent / 'shared/owner_sha256.c'],
        'includes': [FIRMWARE / 'components/fw_settings/include', FIRMWARE.parent.parent / 'shared'],
    },
    'test_schedule': {
        'sources': [HERE / 'test_schedule.c',
                    FIRMWARE / 'components/fw_sync/fw_schedule.c',
                    FIRMWARE / 'components/fw_sync/fw_sync.c',
                    FIRMWARE / 'components/fw_flight/fw_flight.c',
                    FIRMWARE / 'components/fw_flight/fw_rotation.c',
                    FIRMWARE / 'components/fw_screens/fw_screens.c', FIRMWARE / 'components/fw_screens/fw_brand.c',
                    FIRMWARE / 'components/fw_logo/fw_logo.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'third_party': PNGDEC_SOURCES + [FIRMWARE / 'components/pngdec/pngdec_unit.c',
                                         FIRMWARE.parent / 'build/deps/qrcode/src/qrcode.c',
                                         FIRMWARE / 'components/qrcode/fw_qr.c', CJSON / 'cJSON.c'],
        'includes': [FIRMWARE / 'components/fw_sync/include', FIRMWARE / 'components/fw_flight/include',
                     FIRMWARE / 'components/fw_screens/include', FIRMWARE / 'components/fw_logo/include',
                     FIRMWARE / 'components/fw_text/include', PNGDEC, FIRMWARE / 'components/qrcode/include',
                     FIRMWARE.parent / 'build/deps/qrcode/src', CJSON],
        'defines': PNGDEC_DEFINES,
    },
    'test_cloud': {
        'sources': [HERE / 'test_cloud.c', FIRMWARE / 'components/fw_settings/fw_cloud.c'],
        'third_party': [CJSON / 'cJSON.c'],
        'includes': [FIRMWARE / 'components/fw_settings/include', CJSON],
    },
    'test_scroll': {
        'sources': [HERE / 'test_scroll.c',
                    FIRMWARE / 'components/fw_flight/fw_flight.c',
                    FIRMWARE / 'components/fw_screens/fw_screens.c', FIRMWARE / 'components/fw_screens/fw_brand.c',
                    FIRMWARE / 'components/fw_logo/fw_logo.c',
                    FIRMWARE / 'components/fw_text/fw_text.c',
                    FIRMWARE / 'components/fw_text/font5x7.c',
                    FIRMWARE / 'components/fw_text/vendor_charmap.c'],
        'third_party': PNGDEC_SOURCES + [FIRMWARE / 'components/pngdec/pngdec_unit.c',
                                         FIRMWARE.parent / 'build/deps/qrcode/src/qrcode.c',
                                         FIRMWARE / 'components/qrcode/fw_qr.c'],
        'includes': [FIRMWARE / 'components/fw_flight/include', FIRMWARE / 'components/fw_screens/include',
                     FIRMWARE / 'components/fw_logo/include', FIRMWARE / 'components/fw_text/include', PNGDEC,
                     FIRMWARE / 'components/qrcode/include', FIRMWARE.parent / 'build/deps/qrcode/src'],
        'defines': PNGDEC_DEFINES,
    },
    'test_guard': {
        'sources': [HERE / 'test_guard.c'],
        'includes': [FIRMWARE / 'main'],
    },
    'test_link': {
        'sources': [HERE / 'test_link.c', FIRMWARE / 'components/fw_wifi/fw_link.c'],
        'includes': [FIRMWARE / 'components/fw_wifi/include'],
    },
    'test_nets': {
        'sources': [HERE / 'test_nets.c', FIRMWARE / 'components/fw_wifi/fw_nets.c'],
        'third_party': [CJSON / 'cJSON.c'],
        'includes': [FIRMWARE / 'components/fw_wifi/include', CJSON],
    },
}


def main():
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, spec in TESTS.items():
            exe = Path(tmp) / (name + '.exe')
            cmd = [gcc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wno-sign-conversion']
            objs = []
            for src in spec.get('third_party', []):     # vendored code: built as-is, warnings off
                obj = Path(tmp) / (name + '-' + src.stem + '.o')
                tp = subprocess.run([gcc, '-std=gnu11', '-O1', '-w', '-c'] + spec.get('defines', []) +
                                    ['-I%s' % i for i in spec['includes']] + [str(src), '-o', str(obj)],
                                    capture_output=True, text=True)
                if tp.returncode:
                    print('BUILD FAIL %s (%s)\n%s' % (name, src.name, tp.stderr))
                    break
                objs.append(str(obj))
            cmd += spec.get('defines', []) + ['-I%s' % i for i in spec['includes']]
            cmd += [str(s) for s in spec['sources']] + objs + ['-o', str(exe)] + spec.get('libs', [])
            build = subprocess.run(cmd, capture_output=True, text=True)
            if build.returncode:
                print('BUILD FAIL %s\n%s' % (name, build.stderr))
                failed += 1
                continue
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            print(('ok   ' if run.returncode == 0 else 'FAIL ') + name + ': ' + (run.stdout + run.stderr).strip())
            failed += run.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
