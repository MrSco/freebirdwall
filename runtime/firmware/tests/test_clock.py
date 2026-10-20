"""Compare fw_clock's local time and DST with the IANA tz database.

    py runtime/firmware/tests/test_clock.py

The stock DST rules (CLOCK.md) are north_america: 2nd Sunday of March to 1st
Sunday of November at 02:00 local; europe: last Sunday of March to last Sunday
of October at 01:00 UTC. Real zones that follow them must agree with fw_clock
to the second, around every transition (one second either side) and at random
moments. Needs the tzdata package: py -m pip install --target
runtime/build/deps/py-tzdata tzdata
"""
import random
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
TZDATA = FW.parent / 'build' / 'deps' / 'py-tzdata'
sys.path.insert(0, str(TZDATA))
from zoneinfo import ZoneInfo  # noqa: E402

NONE, NA, EU = 0, 1, 2
# (tz name, stock standard offset, stock rule, first year the real zone matches the rule)
ZONES = [
    ('America/New_York', -300, NA, 2007), ('America/Chicago', -360, NA, 2007),
    ('America/Denver', -420, NA, 2007), ('America/Los_Angeles', -480, NA, 2007),
    ('America/Anchorage', -540, NA, 2007), ('America/Halifax', -240, NA, 2007),
    ('America/St_Johns', -210, NA, 2012),
    ('Europe/London', 0, EU, 1997), ('Europe/Lisbon', 0, EU, 1997), ('Europe/Paris', 60, EU, 1997),
    ('Europe/Berlin', 60, EU, 1997), ('Europe/Helsinki', 120, EU, 1997), ('Europe/Athens', 120, EU, 1997),
    ('Asia/Tokyo', 540, NONE, 1970), ('Asia/Kolkata', 330, NONE, 1970), ('Asia/Kathmandu', 345, NONE, 1987),
    ('America/Phoenix', -420, NONE, 1970), ('Pacific/Honolulu', -600, NONE, 1970), ('UTC', 0, NONE, 1970),
]
LAST_YEAR = 2037   # stock time_t is 32-bit


def moments(first_year, rng):
    first = max(first_year, 1971)
    for year in range(first, LAST_YEAR + 1):
        for month, days in ((3, range(8, 15)), (3, range(25, 32)), (10, range(25, 32)), (11, range(1, 8))):
            for day in days:
                for hour in range(24):
                    t = int(datetime(year, month, day, hour, tzinfo=timezone.utc).timestamp())
                    yield from (t - 1, t, t + 1)
    lo = int(datetime(first, 1, 1, tzinfo=timezone.utc).timestamp())
    hi = int(datetime(LAST_YEAR, 12, 31, tzinfo=timezone.utc).timestamp())
    for _ in range(20000):
        yield rng.randrange(lo, hi)


def main():
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    rng = random.Random(20261019)
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'clock_dump.exe'
        srcs = [HERE / 'clock_dump.c', FW / 'components/fw_clock/fw_clock.c', FW / 'components/fw_clock/clock_tables.c',
                FW / 'components/fw_text/fw_text.c', FW / 'components/fw_text/font5x7.c',
                FW / 'components/fw_text/vendor_charmap.c']
        incs = [FW / 'components/fw_clock/include', FW / 'components/fw_clock', FW / 'components/fw_text/include']
        r = subprocess.run([gcc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror'] + ['-I%s' % i for i in incs] +
                           [str(s) for s in srcs] + ['-o', str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        total = failures = 0
        for name, offset, rule, first in ZONES:
            tz = ZoneInfo(name)
            ts = list(moments(first, rng))
            out = subprocess.run([str(exe)], input='\n'.join('%d %d %d' % (t, offset, rule) for t in ts) + '\n',
                                 capture_output=True, text=True, check=True).stdout.split('\n')
            bad = 0
            for t, line in zip(ts, out):
                d = datetime.fromtimestamp(t, tz)
                want = '%d %d %d %d %d %d %d %d' % (d.year, d.month, d.day, d.hour, d.minute, d.second,
                                                      (d.weekday() + 1) % 7, int(d.dst() != timedelta(0)))
                if line != want:
                    if bad < 3:
                        print('  %s t=%d: got %s want %s' % (name, t, line, want))
                    bad += 1
            total += len(ts)
            failures += bad
            print('%-20s %7d moments  %s' % (name, len(ts), 'ok' if not bad else '%d MISMATCHES' % bad))
        print('%s: %d moments, %d mismatches' % ('PASS' if not failures else 'FAIL', total, failures))
        sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
