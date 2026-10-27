"""Desktop tests for components/fw_community: community records (the shared
runtime reader, as the firmware builds it with 8 records) turned
into the screens' flight list (FEED.md "Community feed").

    py tests/test_community.py

Fixtures follow the adsb.fi v3 and adsbdb v0 field layouts; they are made up
here, not captured traffic. FW_COMMUNITY_SAMPLE=<adsb.fi response file> also
runs one real response through it (counts only are printed).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
RUNTIME = FW.parent
checks, failures = 0, []


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)


def build(tmp):
    gcc, gxx = shutil.which('gcc'), shutil.which('g++')
    if not gcc or not gxx:
        sys.exit('desktop gcc and g++ are required')
    inc = ['-I%s' % p for p in (FW / 'components/fw_community/include', RUNTIME / 'src', RUNTIME / 'esp32',
                                FW / 'components/fw_feed/include', FW / 'components/fw_flight/include',
                                FW / 'components/fw_sync/include', FW / 'components/fw_text/include',
                                FW / 'components/fw_screens/include', FW / 'components/fw_logo/include',
                                FW / 'components/qrcode/include', RUNTIME / 'build/deps/qrcode/src',
                                FW / 'components/fw_airports/include',
                                RUNTIME / 'build/deps/pngdec/src')]
    objs = []
    for c in ('fw_flight/fw_flight.c', 'fw_text/fw_text.c', 'fw_text/font5x7.c', 'fw_text/vendor_charmap.c',
              'fw_airports/fw_airports.c', 'fw_airports/airports_table.c'):
        obj = Path(tmp) / (Path(c).stem + '.o')
        r = subprocess.run([gcc, '-std=gnu11', '-O1', '-w', '-c', *inc, str(FW / 'components' / c), '-o', str(obj)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit('%s build failed:\n%s' % (c, r.stderr[-3000:]))
        objs.append(str(obj))
    exe = Path(tmp) / 'community_dump.exe'
    r = subprocess.run([gxx, '-std=gnu++17', '-O1', '-Wall', '-Wextra', '-Werror', '-DFLIGHTWALL_MAX_FLIGHTS=8', *inc,
                        str(HERE / 'community_dump.cpp'), str(FW / 'components/fw_community/fw_community_frame.cpp'),
                        str(FW / 'components/fw_community/fw_aero.cpp'),
                        str(RUNTIME / 'src/flight_data.cpp'), *objs, '-o', str(exe)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stderr[-3000:])
    return exe


def run(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('community_dump %s failed (%d)' % (args[0], r.returncode))
    return json.loads(r.stdout)


def aircraft(**kw):
    a = {'hex': 'a1b2c3', 'type': 'adsb_icao', 'flight': 'DAL1234 ', 'r': 'N123DL', 't': 'B738',
         'desc': 'BOEING 737-800', 'alt_baro': 35000, 'gs': 450.0, 'track': 90.0, 'baro_rate': -1500,
         'category': 'A3', 'lat': 26.7, 'lon': -80.1, 'seen_pos': 1.2, 'seen': 0.5, 'messages': 100,
         'mlat': [], 'tisb': [], 'rssi': -20.1, 'dst': 3.2, 'dir': 120.0, 'squawk': '1200'}
    a.update(kw)
    return {k: v for k, v in a.items() if v is not None}


ROUTE = {'response': {'flightroute': {
    'callsign': 'BAW117', 'callsign_icao': 'BAW117', 'callsign_iata': 'BA117',
    'airline': {'name': 'British Airways', 'icao': 'BAW', 'iata': 'BA', 'country': 'United Kingdom',
                'country_iso': 'GB', 'callsign': 'SPEEDBIRD'},
    'origin': {'country_iso_name': 'GB', 'country_name': 'United Kingdom', 'elevation': 83, 'iata_code': 'LHR',
               'icao_code': 'EGLL', 'latitude': 51.4706, 'longitude': -0.46194, 'municipality': 'London',
               'name': 'London Heathrow Airport'},
    'destination': {'country_iso_name': 'US', 'country_name': 'United States', 'elevation': 13,
                    'iata_code': 'JFK', 'icao_code': 'KJFK', 'latitude': 40.6398, 'longitude': -73.7789,
                    'municipality': 'New York', 'name': 'John F Kennedy International Airport'}}}}


def progress(olat, olon, dlat, dlon, lat, lon):
    """fw_route_progress in the air, worked out independently."""
    from math import asin, cos, radians, sin, sqrt

    def d(a, b, c, e):
        h = sin(radians(c - a) / 2) ** 2 + cos(radians(a)) * cos(radians(c)) * sin(radians(e - b) / 2) ** 2
        return 2 * 6371008.8 * asin(sqrt(h))
    flown, left = d(olat, olon, lat, lon), d(lat, lon, dlat, dlon)
    return 100 * flown / (flown + left)


def near(a, b, tol=0.01):
    return a is not None and abs(a - b) <= tol


def test_frame(exe, tmp):
    rows = [aircraft(),
            aircraft(hex='b00001', flight='BAW117  ', r='G-STBA', t='B77W', desc='BOEING 777-300ER', category='A5',
                     lat=52.0, lon=-35.0),
            aircraft(hex='b00002', flight='N911HL  ', r='N911HL', t='EC35', desc='EUROCOPTER EC-135', category='A7',
                     alt_baro=1200, gs=110.0, baro_rate=0),
            aircraft(hex='b00003', flight='OPS1    ', r='', t='SERV', desc=None, category='C2', alt_baro='ground',
                     gs=13.2, baro_rate=None),
            aircraft(hex='b00004', flight='N12345  ', r='N12345', t='C172', desc='CESSNA 172 SKYHAWK', category='A1',
                     alt_baro=3500, gs=105.0),
            aircraft(hex='b00005', flight='DAL88   '),
            aircraft(hex='b00006', flight='DAL99   ')]
    pos = Path(tmp) / 'pos.json'
    pos.write_text(json.dumps({'now': 1, 'total': len(rows), 'ctime': 1, 'ptime': 0, 'msg': 'No error', 'ac': rows}))
    route = Path(tmp) / 'route.json'
    route.write_text(json.dumps(ROUTE))
    f = run(exe, 'frame', pos, route, 'BAW117')
    check(f['result'] == 0 and f['count'] == 5 and f['overflow'] == 2 and not f['tracked'], 'frame: 5 shown, 2 more %r' % f)
    d, b, h, g, p = f['flights']
    check(d['icao'] == 'A1B2C3' and d['cs'] == 'DAL1234' and d['reg'] == 'N123DL', 'frame: ids %r' % d)
    check(near(d['alt_b'], 10668.0) and near(d['vel'], 231.5, 0.05) and near(d['trk'], 90) and near(d['vr'], -7.62),
          'frame: SI units %r' % d)
    check(d['alt_g'] is None and not d['gnd'], 'frame: geometric altitude unknown, airborne')
    check(d['al_code'] == 'DAL' and d['ac_type'] == 'B738' and d['ac_name'] == 'Boeing 737-800' and d['ac_class'] == '',
          'frame: airline from the callsign, aircraft %r' % d)
    check(d['orig'] == '' and d['dest'] == '' and d['orig_iata'] == '' and d['al_name'] == '', 'frame: no route yet')
    check(not d['has_trk'] and d['prog'] is None and d['sqk'] == '', 'frame: not a tracked flight')
    check(b['orig'] == 'London Heathrow' and b['dest'] == 'John F Kennedy Intl', 'frame: route names %r' % b)
    check((b['orig_iata'], b['orig_icao'], b['dest_iata'], b['dest_icao']) == ('LHR', 'EGLL', 'JFK', 'KJFK'),
          'frame: route codes')
    check(b['al_code'] == 'BAW' and b['al_name'] == 'British Airways' and b['ac_name'] == 'Boeing 777-300ER',
          'frame: route airline %r' % b)
    check(h['ac_class'] == 'Heli' and h['al_code'] == '' and h['ac_name'] == 'Eurocopter EC-135', 'frame: helicopter %r' % h)
    check(g['gnd'] and g['alt_b'] is None and near(g['vel'], 6.79, 0.01) and g['vr'] is None and g['ac_name'] == '',
          'frame: on the ground %r' % g)
    check(p['al_code'] == '' and p['ac_name'] == 'Cessna 172 Skyhawk', 'frame: private aircraft %r' % p)
    # the progress bar: Heathrow (EGLL) to JFK (KJFK) from the airport table, the aircraft mid-Atlantic
    want = progress(51.4708, -0.4599, 40.6394, -73.7793, 52.0, -35.0)
    check(b['prog'] is not None and near(b['prog'], want, 0.05) and not b['has_trk'],
          'frame: route progress %r, want %.2f' % (b['prog'], want))
    rows[1]['lat'], rows[1]['lon'] = 26.7, -80.1            # Florida: far off the London - New York route
    pos.write_text(json.dumps({'ac': rows[:2]}))
    f = run(exe, 'frame', pos, route, 'BAW117')
    check(f['flights'][1]['orig_icao'] == 'EGLL' and f['flights'][1]['prog'] is None, 'frame: off the route, no progress')
    rows[1]['lat'], rows[1]['lon'] = 52.0, -35.0

    # fewer than five: no overflow; empty list
    pos.write_text(json.dumps({'ac': rows[:2]}))
    f = run(exe, 'frame', pos)
    check(f['count'] == 2 and f['overflow'] == 0 and f['flights'][1]['orig'] == '', 'frame: two, no route %r' % f)
    pos.write_text(json.dumps({'ac': []}))
    f = run(exe, 'frame', pos)
    check(f['count'] == 0 and f['overflow'] == 0 and f['flights'] == [], 'frame: empty')
    # a route for another callsign is not applied
    pos.write_text(json.dumps({'ac': rows[1:2]}))
    f = run(exe, 'frame', pos, route, 'BAW118')
    check(f['flights'][0]['orig'] == '', 'frame: route must match the callsign')


def test_names(exe):
    cases = [('London Heathrow Airport', 'London Heathrow'),
             ('John F Kennedy International Airport', 'John F Kennedy Intl'),
             ('Palm Beach Intl Airport', 'Palm Beach Intl'),
             ('Charlotte Douglas International Airport', 'Charlotte Douglas Intl'),
             ('Fort Lauderdale/Hollywood International Airport', 'Fort Lauderdale/Hollywood Intl'),
             ('Heathrow', 'Heathrow'), ('Airport', 'Airport'), ('', ''),
             ('Hartsfield Jackson Atlanta International Airport', 'Hartsfield Jackson Atlanta Intl'),
             ('Hartsfield Jackson Atlanta International Airport Long', 'Hartsfield Jackson Atlanta Inte')]
    for given, want in cases:
        got = run(exe, 'airport', given)
        check(got == want, 'airport: %r -> %r, want %r' % (given, got, want))
    cases = [('BOEING 737-800', 'Boeing 737-800'), ('AIRBUS A-320', 'Airbus A-320'), ('ATR 72-600', 'ATR 72-600'),
             ('EMBRAER ERJ-175', 'Embraer ERJ-175'), ('Boeing 787-9', 'Boeing 787-9'), ('', ''),
             ('DE HAVILLAND CANADA DHC-8', 'De Havilland Canada DHC-8')]
    for given, want in cases:
        got = run(exe, 'aircraft', given)
        check(got == want, 'aircraft: %r -> %r, want %r' % (given, got, want))


def test_track(exe):
    NOW = 1792500000
    dep, arr = NOW - 35 * 60, NOW + 95 * 60
    CLT, PBI = (35.214, -80.943), (26.683, -80.096)
    def tr(gnd, vel, lat, lon, coords=1):
        return run(exe, 'track', int(gnd), vel, lat, lon, coords, CLT[0], CLT[1], PBI[0], PBI[1], dep, arr)

    t = tr(0, 230.0, 32.0, -80.6)                    # in the air, a third of the way
    check(t['has_trk'] and 30 < t['prog'] < 40 and abs(t['dist'] - 358000) < 5000, 'track: progress and distance %r' % t)
    check(t['dep'] == '35m ago', 'track: departed text %r' % t)
    check(t['arr'] == '42m', 'track: arriving in, from ground speed %r' % t)   # 593 km / 230 m/s = 42.98 min, whole minutes down
    t = tr(1, 5.0, CLT[0], CLT[1])                   # on the ground at the origin
    check(t['prog'] == 0 and t['trk_gnd'], 'track: still at the origin %r' % t)
    t = tr(1, 5.0, PBI[0] + 0.02, PBI[1])            # down at the destination
    check(t['prog'] == 100, 'track: landed %r' % t)
    t = tr(0, 230.0, 32.0, -80.6, coords=0)          # no airport coordinates: from the schedule
    check(t['dist'] is None and abs(t['prog'] - 26.9) < 0.2 and t['arr'] == '1h 35m', 'track: schedule fallback %r' % t)


def aero_flight(**kw):
    f = {'ident': 'AAL100', 'fa_flight_id': 'AAL100-1792400000-schedule-0001', 'cancelled': False,
         'registration': 'N101NN', 'aircraft_type': 'A321', 'progress_percent': 0, 'status': 'Scheduled',
         'route': 'A LONG ROUTE STRING THE FILTER DROPS', 'codeshares': ['BAW1500', 'IBE4218'],
         'origin': {'code': 'KJFK', 'code_icao': 'KJFK', 'code_iata': 'JFK', 'code_lid': 'JFK',
                    'timezone': 'America/New_York', 'name': 'John F Kennedy Intl', 'city': 'New York',
                    'airport_info_url': '/airports/KJFK'},
         'destination': {'code': 'EGLL', 'code_icao': 'EGLL', 'code_iata': 'LHR', 'code_lid': None,
                         'timezone': 'Europe/London', 'name': 'London Heathrow', 'city': 'London',
                         'airport_info_url': '/airports/EGLL'},
         'scheduled_out': None, 'scheduled_off': None, 'estimated_off': None, 'actual_off': None,
         'scheduled_on': None, 'scheduled_in': None, 'estimated_on': None, 'actual_on': None}
    f.update(kw)
    return f


def iso(t):
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def test_aero(exe, tmp):
    now = 1792500000
    check(run(exe, 'aerotime', '2026-10-20T14:05:00Z') == 1792505100, 'aero: time')
    check(run(exe, 'aerotime', '2026-10-20T14:05:00.123Z') == 1792505100, 'aero: fractional seconds')
    check(run(exe, 'aerotime', '2026-10-20T16:05:00+02:00') == 1792505100, 'aero: offset')
    check(run(exe, 'aerotime', 'soon') == 0 and run(exe, 'aerotime', '2026-13-01T00:00:00Z') == 0, 'aero: not a time')

    def pick(flights):
        p = Path(tmp) / 'aero.json'
        p.write_text(json.dumps({'flights': flights, 'links': None, 'num_pages': 1}))
        return run(exe, 'aero', p, now)

    # newest first, as FlightAware lists them: tomorrow's, today's in the air, yesterday's landed
    tomorrow = aero_flight(fa_flight_id='T', scheduled_out=iso(now + 86000), scheduled_off=iso(now + 86600),
                           scheduled_on=iso(now + 112000))
    flying = aero_flight(fa_flight_id='F', scheduled_off=iso(now - 3600), actual_off=iso(now - 3300),
                         estimated_on=iso(now + 20000), scheduled_on=iso(now + 19000), progress_percent=14)
    old = aero_flight(fa_flight_id='Y', actual_off=iso(now - 90000), actual_on=iso(now - 64000))
    a = pick([tomorrow, flying, old])
    check(a['ok'] and a['airborne'] and a['fa_id'] == 'F' and a['dep'] == now - 3300 and a['arr'] == now + 20000,
          'aero: the flight in the air %r' % a)
    check(a['progress'] == 14 and a['orig_iata'] == 'JFK' and a['orig_icao'] == 'KJFK' and
          a['orig'] == 'John F Kennedy Intl' and a['dest_iata'] == 'LHR' and a['ac_type'] == 'A321' and a['reg'] == 'N101NN',
          'aero: airports and aircraft %r' % a)
    check(a['doc_bytes'] < 3000, 'aero: the filtered document stays small (%d bytes)' % a['doc_bytes'])
    # landed 10 minutes ago beats tomorrow's
    just = aero_flight(fa_flight_id='L', actual_off=iso(now - 20000), actual_on=iso(now - 600))
    a = pick([tomorrow, just, old])
    check(a['ok'] and a['landed'] and not a['airborne'] and a['fa_id'] == 'L' and a['arr'] == now - 600, 'aero: just landed %r' % a)
    # landed long ago: the next one
    a = pick([tomorrow, old])
    check(a['ok'] and not a['airborne'] and not a['landed'] and a['fa_id'] == 'T' and a['dep'] == now + 86600,
          'aero: next departure (runway time first) %r' % a)
    # the soonest of two upcoming; a delayed one 2 h late still counts; cancelled ones never
    later = aero_flight(fa_flight_id='T2', scheduled_off=iso(now + 30000))
    late = aero_flight(fa_flight_id='D', scheduled_off=iso(now - 7200), estimated_off=iso(now + 1200))
    gone = aero_flight(fa_flight_id='C', cancelled=True, scheduled_off=iso(now + 600))
    a = pick([later, gone, late])
    check(a['fa_id'] == 'D' and a['dep'] == now + 1200, 'aero: delayed, then the soonest; cancelled skipped %r' % a)
    stale = aero_flight(fa_flight_id='S', scheduled_off=iso(now - 4 * 3600))
    a = pick([stale])
    check(not a['ok'], 'aero: never left and 4 h past its time: none %r' % a)
    a = pick([aero_flight(fa_flight_id='G', scheduled_out=iso(now + 900))])
    check(a['ok'] and a['dep'] == now + 900, 'aero: gate time when the runway time is missing %r' % a)
    check(not pick([])['ok'], 'aero: no flights')

    p = Path(tmp) / 'pos.json'
    p.write_text(json.dumps({'ident': 'AAL100', 'waypoints': [40.1, -73.2] * 400, 'last_position': {
        'fa_flight_id': 'F', 'altitude': 350, 'altitude_change': '-', 'groundspeed': 480, 'heading': 62,
        'latitude': 51.2, 'longitude': -30.5, 'timestamp': iso(now - 30), 'update_type': 'O'}}))
    q = run(exe, 'aeropos', p)
    check(q['ok'] and q['lat'] == 51.2 and q['lon'] == -30.5 and abs(q['alt'] - 10668) < 1 and abs(q['gs'] - 246.93) < 0.1
          and q['heading'] == 62, 'aero: position in metres and m/s %r' % q)
    check(q['doc_bytes'] < 400, 'aero: waypoints filtered out (%d bytes)' % q['doc_bytes'])
    p.write_text(json.dumps({'last_position': None}))
    check(not run(exe, 'aeropos', p)['ok'], 'aero: no position')


def test_sample(exe):
    sample = os.environ.get('FW_COMMUNITY_SAMPLE')
    if not sample:
        return
    f = run(exe, 'frame', sample)
    check(f['result'] == 0 and 0 <= f['count'] <= 5, 'sample: decoded')
    print('sample: %d shown, %d more' % (f['count'], f['overflow']))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        test_frame(exe, tmp)
        test_names(exe)
        test_track(exe)
        test_aero(exe, tmp)
        test_sample(exe)
    for f in failures:
        print('FAIL', f)
    print('%s: %d community checks, %d failed' % ('PASS' if not failures else 'FAIL', checks, len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
