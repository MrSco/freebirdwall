"""Makes the airport locations table from OurAirports (public domain).

    py runtime/firmware/tools/make_airports.py <airports.csv>

airports.csv: https://davidmegginson.github.io/ourairports-data/airports.csv
(not committed; about 13 MB). Writes components/fw_airports/airports_table.c:
every large, medium and small airport that has a four-letter code, indexed by
each of its codes (ident, icao_code, gps_code: a renamed airport, such as
KPBI now KDJT, is found by either), sorted for a binary search. A record is
10 bytes: the code, then latitude and longitude in 1/10,000 of a degree
(about 11 m) as signed 24-bit numbers, little-endian. Used for the progress
bar of FlightWall-feed flights (FLIGHT-SCREENS.md "Progress bar").
"""
import csv
import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
OUT = FW / 'components' / 'fw_airports' / 'airports_table.c'
TYPES = ('large_airport', 'medium_airport', 'small_airport')
NL = chr(10)


def s24(v):
    v = int(round(v * 10000))
    assert -(1 << 23) <= v < (1 << 23)
    return [v & 255, (v >> 8) & 255, (v >> 16) & 255]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    table = {}
    with open(sys.argv[1], encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f):
            if r['type'] not in TYPES:
                continue
            try:
                lat, lon = float(r['latitude_deg']), float(r['longitude_deg'])
            except ValueError:
                continue
            rank = TYPES.index(r['type'])
            for k in ('icao_code', 'gps_code', 'ident'):
                code = r[k].strip().upper()
                if re.fullmatch(r'[A-Z]{4}', code) and (code not in table or rank < table[code][0]):
                    table[code] = (rank, lat, lon)
    codes = sorted(table)
    data = []
    for c in codes:
        data += [ord(x) for x in c] + s24(table[c][1]) + s24(table[c][2])
    rows = (',' + NL).join('    ' + ','.join(str(b) for b in data[i:i + 30]) for i in range(0, len(data), 30))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(NL.join((
        '/* Airport locations, made by tools/make_airports.py from OurAirports',
        ' * (https://ourairports.com/data/, public domain); do not edit. %d codes,' % len(codes),
        ' * sorted: 4 letters, then latitude and longitude in 1/10,000 degree, signed',
        ' * 24-bit little-endian. */',
        '#include <stdint.h>',
        '',
        'const unsigned fw_airport_count = %d;' % len(codes),
        'const uint8_t fw_airport_table[%d] = {' % len(data),
        rows,
        '};',
        '')), encoding='utf-8', newline=NL)
    print('wrote %s: %d codes, %d bytes' % (OUT, len(codes), len(data)))


if __name__ == '__main__':
    main()
