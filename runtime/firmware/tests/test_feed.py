"""Desktop tests for components/fw_feed: the vendor feed response parser, the
request body and the display settings (FEED.md).

    py tests/test_feed.py

Fixtures are encoded with the independent msgpack library (installed in
build/deps/py-msgpack: py -m pip install --target
runtime/build/deps/py-msgpack msgpack==1.1.0); expectations follow
the stock rules in FEED.md.
"""
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
DEPS = FW.parent / 'build' / 'deps'
sys.path.insert(0, str(DEPS / 'py-msgpack'))
import msgpack  # noqa: E402

PNGDEC = DEPS / 'pngdec' / 'src'
CJSON = Path.home() / 'esp' / 'esp-idf-v4.4.7' / 'components' / 'json' / 'cJSON'
SOURCES = [HERE / 'feed_dump.c', FW / 'components/fw_feed/fw_feed_msgpack.c', FW / 'components/fw_feed/fw_feed_config.c',
           FW / 'components/fw_feed/fw_feed_http.c',
           FW / 'components/fw_flight/fw_flight.c', FW / 'components/fw_flight/fw_rotation.c',
           FW / 'components/fw_sync/fw_sync.c', FW / 'components/fw_screens/fw_screens.c', FW / 'components/fw_screens/fw_brand.c',
           FW / 'components/fw_logo/fw_logo.c', FW / 'components/fw_text/fw_text.c',
           FW / 'components/fw_text/font5x7.c', FW / 'components/fw_text/vendor_charmap.c',
           FW / 'components/pngdec/pngdec_unit.c', FW / 'components/qrcode/fw_qr.c',
           FW.parent / 'build/deps/qrcode/src/qrcode.c', CJSON / 'cJSON.c'] + [
    PNGDEC / n for n in ('adler32.c', 'crc32.c', 'infback.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'zutil.c')]
INCLUDES = [FW / 'components' / c / 'include' for c in ('fw_feed', 'fw_flight', 'fw_sync', 'fw_screens', 'fw_logo',
                                                        'fw_text', 'qrcode')] + [
    PNGDEC, FW.parent / 'build/deps/qrcode/src', CJSON]

checks = 0
failures = []


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)


def build(tmp):
    gcc = shutil.which('gcc') or sys.exit('a desktop gcc is required')
    exe = Path(tmp) / 'feed_dump.exe'
    cmd = [gcc, '-std=gnu11', '-O1', '-w', '-D__LINUX__=1', '-Duint=unsigned',
           '-DPNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)'] + ['-I%s' % i for i in INCLUDES] + [str(s) for s in SOURCES] + [
        '-o', str(exe), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stderr[-3000:])
    return exe


def run(exe, tmp, mode, data):
    p = Path(tmp) / ('in.' + mode)
    p.write_bytes(data if isinstance(data, bytes) else data.encode())
    r = subprocess.run([str(exe), mode, str(p)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('feed_dump %s failed' % mode)
    out = r.stdout.strip()
    return out if mode == 'body' else json.loads(out)


def text(s):
    """What the dump prints for raw bytes (each byte >= 0x80 as \\u00xx)."""
    return s.decode('latin-1') if isinstance(s, bytes) else s


FULL = {'icao': 'a1b2c3', 'cs': 'AAL1234', 'reg': 'n915nn', 'alt_b': 10668.0, 'alt_g': 10700.5, 'vel': 231.5,
        'trk': 348.0, 'vr': -5.5, 'gnd': False, 'sqk': '1200', 'orig_icao': 'KCLT', 'orig_iata': 'CLT',
        'dest_icao': 'KPBI', 'dest_iata': 'PBI', 'orig': 'Charlotte Douglas', 'dest': 'Palm Beach Intl',
        'al_code': 'AAL', 'al_paint': 'AAL', 'al_op': 'AAL', 'al_name': 'American', 'ac_type': 'B738',
        'ac_name': '737-800', 'ac_class': 'Commercial',
        'deltas': {'alt_b': 1.0, 'alt_g': 2.0, 'vel': 3.0, 'trk': 4.0, 'vr': 5.0, 'dt': 6.0}}


def mp(obj, **kw):
    return msgpack.packb(obj, use_bin_type=True, **kw)


def test_full(exe, tmp):
    leg = {'lt': 'operating', 'ls': '', 'al': 'AAL', 'fn': '1234', 'orig': 'CLT', 'dest': 'PBI',
           'dep_rel': 'in 2h 05m', 'arr_rel': 'in 4h', 'dep_at': '2026-10-19T14:30:00Z', 'arr_at': '2026-10-19T16:30:00Z'}
    body = mp({'v': 1, 'f': [FULL, {'cs': 'DAL42'}], 'ch': 'abc123', 'ov': 7, 'sla': leg, 'slu': [leg, dict(leg, fn='99')],
               'scm': 'Home for dinner'})
    r = run(exe, tmp, 'parse', body)
    check(r['rc'] == 0 and r['count'] == 2 and not r['tracked'] and r['overflow'] == 7 and r['ch'] == 'abc123', 'full: envelope')
    f = r['flights'][0]
    for k, v in FULL.items():
        if k == 'reg':
            check(f[k] == 'N915NN', 'full: reg upper-cased')
        elif k != 'deltas':
            check(f[k] == v, 'full: %s %r != %r' % (k, f[k], v))
    check(not f['has_trk'] and f['prog'] is None, 'full: no trk_info')
    g = r['flights'][1]
    check(g['cs'] == 'DAL42' and g['alt_b'] is None and g['icao'] == '', 'full: missing fields NaN/empty')
    check(r['has_active'] and r['active']['fn'] == '1234' and r['active']['dep_at'] == 1792420200, 'full: active leg')
    check([u['fn'] for u in r['upcoming']] == ['1234', '99'], 'full: upcoming legs')
    check(r['message'] == 'Home for dinner', 'full: message')


def test_tracked(exe, tmp):
    trk = {'sts': 'D', 'sexp': 1792420200, 'prog': 42.5, 'dist': 812000.0, 'conf': 'high', 'dep': '2:05 PM',
           'arr': '45m', 'over': 'Richmond', 'tft': '1h 20m', 'gnd': True}
    r = run(exe, tmp, 'parse', mp({'tf': [dict(FULL, trk_info=trk)], 'ov': 0}))
    f = r['flights'][0]
    check(r['rc'] == 0 and r['tracked'], 'tracked: flag')
    check(f['has_trk'] and f['sts'] == 'D' and f['sexp'] == 1792420200 and f['prog'] == 42.5 and f['dist'] == 812000.0,
          'tracked: numbers')
    check((f['conf'], f['dep'], f['arr'], f['over'], f['tft'], f['trk_gnd']) ==
          ('high', '2:05 PM', '45m', 'Richmond', '1h 20m', True), 'tracked: texts')
    for sts, want in (('C', 'C'), ('X', ''), ('CC', ''), ('', '')):
        r = run(exe, tmp, 'parse', mp({'tf': [{'trk_info': {'sts': sts}}]}))
        check(r['flights'][0]['sts'] == want and r['flights'][0]['has_trk'], 'tracked: sts %r' % sts)
    for sexp, want in ((0, 0), (1, 1), (2 ** 32 - 1, 2 ** 32 - 1), (2 ** 32, 0)):
        r = run(exe, tmp, 'parse', mp({'tf': [{'trk_info': {'sexp': sexp}}]}))
        check(r['rc'] == 0 and r['flights'][0]['sexp'] == want, 'tracked: sexp %d' % sexp)
    # the last of f/tf decides the flag; both append
    r = run(exe, tmp, 'parse', mp({'tf': [{'cs': 'A'}], 'f': [{'cs': 'B'}]}))
    check(r['count'] == 2 and not r['tracked'], 'tracked: f after tf')


def test_limits(exe, tmp):
    r = run(exe, tmp, 'parse', mp({'f': [{'cs': 'F%d' % i} for i in range(7)], 'ov': 2}))
    check(r['rc'] == 0 and r['count'] == 5 and r['flights'][4]['cs'] == 'F4', 'limits: five records')
    long = {'cs': 'ABCDEFGHIJKLMNOP', 'icao': '1234567890', 'orig': 'x' * 40, 'al_name': 'é' * 20, 'ac_class': 'c' * 30}
    f = run(exe, tmp, 'parse', mp({'f': [long]}))['flights'][0]
    check(f['cs'] == 'ABCDEFGHIJK' and f['icao'] == '1234567' and f['orig'] == 'x' * 31, 'limits: bytewise cuts')
    check(f['al_name'] == text(('é' * 20).encode()[:23]), 'limits: UTF-8 cut mid-character (stock)')
    check(f['ac_class'] == 'c' * 15, 'limits: ac_class 15 as stock')
    for reg, want in ((' n123ab ', 'N123AB'), ('N12 3', ''), ('A' * 15, 'A' * 15), ('A' * 16, ''), ('G-EZAB', 'G-EZAB'),
                      ('N1_2', ''), ('', '')):
        f = run(exe, tmp, 'parse', mp({'f': [{'reg': reg}]}))['flights'][0]
        check(f['reg'] == want, 'limits: reg %r -> %r (got %r)' % (reg, want, f['reg']))
    msg = ('a' * 253 + 'é').encode()   # 255 bytes: the é is cut mid-way at 255 -> repaired
    r = run(exe, tmp, 'parse', mp({'scm': ('a' * 254 + 'é').encode().decode()}))
    check(r['message'] == 'a' * 254, 'limits: scm partial UTF-8 removed')
    r = run(exe, tmp, 'parse', mp({'scm': msg.decode()}))
    check(r['message'] == text(msg), 'limits: scm complete character kept')
    for ov, want in ((300, 300), (70000, 70000 & 0xffff)):
        check(run(exe, tmp, 'parse', mp({'ov': ov}))['overflow'] == want, 'limits: ov %d' % ov)


def test_types(exe, tmp):
    ok = [{'f': [{'alt_b': None, 'cs': None, 'gnd': None}]},         # nil reads as absent
          {'f': [{'alt_b': 10668}]},                                  # an integer as a double
          {'f': [{'vr': -3}]},                                        # negative fixint as a double
          {'f': [{'gnd': 1}]},                                        # positive fixint as a bool
          {'f': [{'unknown': {'deep': [1, 2, {'x': 1.5}]}}], 'zz': [None, True, 'x', b'bin']},
          {'v': 1}, {'v': 0}, {}]
    for o in ok:
        check(run(exe, tmp, 'parse', mp(o))['rc'] == 0, 'types: accepted %r' % (o,))
    r = run(exe, tmp, 'parse', mp({'f': [{'alt_b': None, 'gnd': 1}]}))
    check(r['flights'][0]['alt_b'] is None and r['flights'][0]['gnd'], 'types: nil/fixint values')
    bad = [{'ov': True}, {'ov': 1.0}, {'ov': -1}, {'v': 'one'}, {'ch': 5}, {'f': [{'cs': 7}]},
           {'f': [{'alt_b': 'high'}]}, {'f': [{'gnd': 'yes'}]}, {'f': 'flights'}, {'f': [5]},
           {'f': [{'trk_info': {'sts': 1}}]}, {'f': [{'trk_info': {'sexp': 1.5}}]}, {'slu': {'fn': '1'}}]
    for o in bad:
        r = run(exe, tmp, 'parse', mp(o))
        check(r['rc'] == -1 and r['count'] == 0, 'types: rejected %r' % (o,))
    check(run(exe, tmp, 'parse', mp({'v': 2, 'f': [{'cs': 'A'}]}))['rc'] == -2, 'types: v > 1')
    check(run(exe, tmp, 'parse', mp([1, 2]))['rc'] == -1, 'types: root array')
    check(run(exe, tmp, 'parse', mp({'f': [FULL]})[:-5])['rc'] == -1, 'types: truncated')
    check(run(exe, tmp, 'parse', mp({'x': msgpack.ExtType(1, b'ab')}))['rc'] == -1, 'types: ext rejected')
    deep = 1
    for _ in range(15):
        deep = [deep]
    check(run(exe, tmp, 'parse', mp({'x': deep}))['rc'] == 0, 'types: nesting 16 skipped')
    for _ in range(3):
        deep = [deep]
    check(run(exe, tmp, 'parse', mp({'x': deep}))['rc'] == -1, 'types: nesting > 16 rejected')
    # keys are compared after truncation: top level 3 characters, records 11
    r = run(exe, tmp, 'parse', mp({'flights': [{'cs': 'A'}], 'chain': 'x', 'ovx': 3}))
    check(r['rc'] == 0 and r['count'] == 0 and r['ch'] == '' and r['overflow'] == 0, 'types: long top keys skipped')
    r = run(exe, tmp, 'parse', mp({'f': [{'ac_class_extra': 'x', 'cs_': 'y'}]}))
    check(r['flights'][0]['ac_class'] == '' and r['flights'][0]['cs'] == '', 'types: record key truncation')
    # floats as f32
    r = run(exe, tmp, 'parse', mp({'f': [{'trk': 90.5}]}, use_single_float=True))
    check(r['flights'][0]['trk'] == 90.5, 'types: f32')


CONFIG = {
    'version': 2,
    'request_config': {
        'radius_requests': [{'id': 'z%d' % i, 'type': 'radius', 'latitude': 40.5 + i, 'longitude': -74.25,
                             'radius_km': 25, 'min_altitude': 500.7, 'max_altitude': -1, 'name': 'Home' if i == 0 else ''}
                            for i in range(7)],
        'geo_requests': [{'coordinates': [[40.1, -74.2], [40.3, -74.0], 'bad', [1]], 'max_altitude': 3000}],
        'data_filters': {'show_light_ac': False, 'min_speed_mps': 20, 'max_speed_mps': -5,
                         'airline_icao_filter': ['aal', 'DAL', '', 'UA', 'jbu'] + ['X%02d' % i for i in range(40)]},
        'tracked_flights': [{'flight_number': 'AA1234567890123', 'created_at': '2026-10-19T14:30:00.000000+00:00',
                             'show_metrics': True}] * 10,
    },
    'display_config': {
        'metric_display': {'alt_b_unit': 'feet', 'vel_unit': 'knots', 'vr_unit': 'fpm', 'trk_unit': 'cardinal',
                           'dist_unit': 'miles'},
        'title_text': 'callsign', 'layout_template': 'LARGE_LOGO', 'loading_template': 'CLOCK',
        'flight_display_speed': 'slow', 'max_flights_behavior': 'indicator_led', 'border_enable': True,
        'border_brightness_percent': 30, 'brightness_percent': 15, 'anchor_airports': ['KCLT', 'CLT', ''],
        'time_zone': {'standard_utc_offset_minutes': -300, 'dst_rule': 'north_america'},
        'clock_is_24hr': True, 'clock_show_seconds': True,
        'clock': {'style': 'LARGE', 'show_date': True, 'additional_timezones': [
            {'label': 'London', 'time_zone': {'standard_utc_offset_minutes': 0, 'dst_rule': 'europe'}},
            {'label': 'Tokyo', 'time_zone': {'standard_utc_offset_minutes': 540}},
            {'label': 'Third', 'time_zone': {'standard_utc_offset_minutes': 60}}]},
        'locale': 'de', 'date_format': {'order': 'day_month', 'style': 'short', 'show_weekday': True},
        'display_enabled': False, 'sleep_time': {'start': '23:15', 'end': '06:30', 'sleep_brightness_percent': 5},
    },
}


def test_body(exe, tmp):
    body = run(exe, tmp, 'body', json.dumps(CONFIG))
    b = json.loads(body, object_pairs_hook=lambda kv: kv)          # keep key order
    keys = [k for k, _ in b]
    check(keys == ['radius_request', 'geo_request', 'radius_requests', 'geo_requests', 'data_filters',
                   'tracked_flights'], 'body: key order %r' % keys)
    d = json.loads(body)
    zones = d['radius_requests']
    check(len(zones) == 5 and d['radius_request'] == zones[0], 'body: five zones, singular = first')
    z0 = json.loads(json.dumps(dict(b)['radius_requests'][0]), object_pairs_hook=lambda kv: kv)
    check([k for k, _ in dict(b)['radius_requests'][0]] == ['type', 'latitude', 'longitude', 'radius_km', 'min_altitude',
                                                            'name'], 'body: zone key order')
    check(zones[0] == {'type': 'radius', 'latitude': 40.5, 'longitude': -74.25, 'radius_km': 25, 'min_altitude': 500,
                       'name': 'Home'}, 'body: zone %r' % zones[0])
    check('name' not in zones[1] and 'max_altitude' not in zones[1] and 'id' not in zones[1], 'body: zone omissions')
    g = d['geo_requests'][0]
    check(g == {'type': 'geo', 'coordinates': [[40.1, -74.2], [40.3, -74.0]], 'max_altitude': 3000} and
          d['geo_request'] == g, 'body: geo %r' % g)
    f = d['data_filters']
    check([k for k, _ in dict(b)['data_filters']][:6] == ['show_light_ac', 'show_heli_ac', 'show_commercial_ac',
                                                          'show_smalljet_ac', 'show_other_ac', 'show_na_routes'],
          'body: filter order')
    check(f['show_light_ac'] is False and f['show_heli_ac'] is True and f['min_speed_mps'] == 20 and
          'max_speed_mps' not in f, 'body: filters %r' % f)
    codes = f['airline_icao_filter']
    check(codes[:3] == ['AAL', 'DAL', 'JBU'] and len(codes) == 32, 'body: airline codes %r' % codes[:4])
    t = d['tracked_flights']
    check(len(t) == 8 and t[0] == {'flight_number': 'AA123456789', 'created_at': '2026-10-19T14:30:00.000000+',   # 27 characters
                                   'auto_remove': True, 'show_distance_travelled': False, 'show_metrics': True},
          'body: tracked %r' % t[0])
    # singular only, legacy top level, nothing
    one = run(exe, tmp, 'body', json.dumps({'request_config': {'radius_request': {'latitude': 1, 'longitude': 2}}}))
    d = json.loads(one)
    check(d == {'radius_request': {'type': 'radius', 'latitude': 1, 'longitude': 2, 'radius_km': 50},
                'radius_requests': [{'type': 'radius', 'latitude': 1, 'longitude': 2, 'radius_km': 50}]},
          'body: singular only %r' % d)
    legacy = run(exe, tmp, 'body', json.dumps({'request_config': {'type': 'radius', 'latitude': 3, 'longitude': 4,
                                                                  'radius_km': 10}}))
    check(json.loads(legacy)['radius_requests'][0]['radius_km'] == 10, 'body: legacy top level')
    check(json.loads(run(exe, tmp, 'body', json.dumps({'request_config': {}}))) == {}, 'body: empty request_config')
    check(run(exe, tmp, 'body', json.dumps({'display_config': {}})) == '-1', 'body: no request_config')


def test_display(exe, tmp):
    d = run(exe, tmp, 'display', json.dumps(CONFIG))
    check(d['scroll'] == 40, 'display: scroll speed normal by default %r' % d['scroll'])
    for word, ms in (('slow', 80), ('normal', 40), ('fast', 20), ('warp', 40)):
        e = run(exe, tmp, 'display', json.dumps(dict(CONFIG, owned_config={'scroll_speed': word})))
        check(e['scroll'] == ms, 'display: scroll speed %s -> %d ms a pixel (%r)' % (word, ms, e['scroll']))
    check(d['has'] and (d['alt'], d['vel'], d['vr'], d['dist'], d['cardinal']) == (1, 2, 3, 2, True), 'display: units %r' % d)
    check(d['title'] == 1 and d['layout'] == 'LARGE_LOGO' and d['loading'] == 'CLOCK' and d['dwell'] == 5000, 'display: text')
    check(d['max'] == 2 and d['border'] and d['border_percent'] == 30 and d['brightness'] == 15, 'display: frame')
    check(d['anchors'] == ['KCLT', 'CLT'], 'display: anchors')
    check((d['offset'], d['dst'], d['h24'], d['seconds'], d['large'], d['date'], d['locale']) ==
          (-300, 'north_america', True, True, True, True, 'de'), 'display: clock %r' % d)
    check(d['day_first'] and d['weekday'], 'display: date format')
    check(d['extras'] == [['London', 0, 'europe'], ['Tokyo', 540, '']], 'display: extra zones, at most two %r' % d['extras'])
    check(not d['enabled'] and d['has_sleep'] and d['sleep'] == [23 * 60 + 15, 6 * 60 + 30, 5], 'display: power %r' % d)
    e = run(exe, tmp, 'display', json.dumps({'request_config': {}}))
    check(not e['has'] and e['layout'] == 'DEFAULT' and e['loading'] == 'TEXT' and e['dwell'] == 3000 and
          e['locale'] == 'en' and e['title'] == 0 and e['enabled'] and not e['has_sleep'], 'display: defaults %r' % e)


def test_zones(exe, tmp):
    UNSET = -32768
    def disp(dc):
        return run(exe, tmp, 'display', json.dumps({'display_config': dc}))
    d = disp({'layout_template': 'DEFAULT'})
    check(d['offset'] == UNSET and d['extras'] == [], 'zones: none given is "no zone" %r' % d)
    check(disp({'clock_utc_offset_minutes': -360})['offset'] == -360, 'zones: the older key')
    check(disp({'clock_utc_offset_minutes': -360, 'time_zone': {'standard_utc_offset_minutes': 60}})['offset'] == 60,
          'zones: time_zone wins')
    check(disp({'time_zone': {'dst_rule': 'europe'}})['offset'] == UNSET, 'zones: no offset is no zone')
    check(disp({'time_zone': {'standard_utc_offset_minutes': 'x'}})['offset'] == UNSET, 'zones: wrong type')
    d = disp({'clock': {'additional_timezones': ['oops', {'label': 'Paris'},
                                                 {'time_zone': {'standard_utc_offset_minutes': 330}}]}})
    check(d['extras'] == [['Paris', UNSET, ''], ['', 330, '']], 'zones: non-objects skipped, missing parts %r' % d['extras'])
    d = disp({'clock': {'additional_timezones': [{'label': 'A very long label indeed', 'time_zone': {
        'standard_utc_offset_minutes': 345, 'dst_rule': 'north_america'}}]}})
    check(d['extras'] == [['A very long lab', 345, 'north_america']], 'zones: label cut to 15 %r' % d['extras'])
    check(disp({'clock': {'additional_timezones': {}}})['extras'] == [], 'zones: not a list')


def test_body_sync(exe, tmp):
    def body(flight, config):
        return run(exe, tmp, 'bodysync', flight + chr(10) + json.dumps(config))
    base = {'request_config': {'radius_requests': [{'latitude': 1, 'longitude': 2}]}}
    d = body('AA1234', base)
    check(d['tracked_flights'] == [{'flight_number': 'AA1234', 'auto_remove': False, 'show_distance_travelled': True,
                                    'show_metrics': True}], 'body sync: the leg added %r' % d.get('tracked_flights'))
    check(d['radius_requests'][0]['latitude'] == 1, 'body sync: the areas stay')
    check('tracked_flights' not in body('', base), 'body sync: no leg, nothing added')
    check('tracked_flights' not in body('aa1234', base), 'body sync: an invalid number is left out')
    check('tracked_flights' not in body('A', base), 'body sync: too short')
    own = {'request_config': {'tracked_flights': [{'flight_number': 'DL1'}, {'flight_number': 'AA1234'}]}}
    t = body('aa1234'.upper(), own)['tracked_flights']
    check([x['flight_number'] for x in t] == ['DL1', 'AA1234'] and t[1]['auto_remove'] is True,
          'body sync: already tracked by the owner, kept as is %r' % t)
    full = {'request_config': {'tracked_flights': [{'flight_number': 'DL%d' % i} for i in range(1, 9)]}}
    t = body('UA77', full)['tracked_flights']
    check(len(t) == 8 and t[7]['flight_number'] == 'UA77' and t[6]['flight_number'] == 'DL7',
          'body sync: with eight tracked the leg takes the last place %r' % [x['flight_number'] for x in t])
    check(body('UA77', {'display_config': {}}) == -1, 'body sync: no request_config, no body')


def test_source(exe, tmp):
    def src(doc):
        return run(exe, tmp, 'source', doc if isinstance(doc, str) else json.dumps(doc))
    check(src({'owned_config': {'flight_source': 'community'}, 'request_config': {}}) == 1, 'source: community')
    check(src({'owned_config': {'flight_source': 'vendor'}}) == 0, 'source: vendor')
    check(src({'request_config': {}}) == 0, 'source: default vendor')
    check(src({'owned_config': {'flight_source': 'COMMUNITY'}}) == 0, 'source: exact spelling only')
    check(src({'owned_config': {'flight_source': 1}}) == 0, 'source: wrong type')
    check(src('not json') == 0, 'source: unreadable document')

    def follow(doc):
        return run(exe, tmp, 'follow', doc if isinstance(doc, str) else json.dumps(doc))
    check(follow({'request_config': {}}) == 1, 'follow app: default on')
    check(follow({'owned_config': {'follow_app': False}}) == 0, 'follow app: off')
    check(follow({'owned_config': {'follow_app': True}}) == 1, 'follow app: on')
    check(follow({'owned_config': {'follow_app': 0}}) == 1, 'follow app: only false turns it off')
    check(follow('not json') == 1, 'follow app: unreadable document')


def test_http(exe, tmp):
    body = mp({'f': [FULL], 'ov': 0})
    ok = (b'HTTP/1.1 200 OK\r\nContent-Type: application/x-msgpack\r\nContent-Length: %d\r\n'
          b'Connection: close\r\n\r\n' % len(body)) + body
    r = run(exe, tmp, 'http', ok)
    check(r['rc'] == 1 and r['status'] == 200 and r['msgpack'] and r['body'] == len(body), 'http: msgpack %r' % r)
    r = run(exe, tmp, 'http', ok.replace(b'application/x-msgpack', b'application/json'))
    check(r['rc'] == 1 and not r['msgpack'], 'http: json type')
    r = run(exe, tmp, 'http', b'HTTP/1.1 401 Unauthorized\r\ncontent-length: 2\r\n\r\n{}')
    check(r['rc'] == 1 and r['status'] == 401, 'http: status kept for the caller')
    r = run(exe, tmp, 'http', b'HTTP/1.0 200 OK\nContent-Length:3\n\nabc')
    check(r['rc'] == 1 and r['body'] == 3, 'http: bare LF and HTTP/1.0')
    refused = [b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\nabc',
               b'HTTP/1.1 200 OK\r\n\r\nabc',
               b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n',
               b'HTTP/1.1 200 OK\r\nContent-Length: 5121\r\n\r\n',
               b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nabc',
               b'HTTP/1.1 200 OK\r\nContent-Length: 2x\r\n\r\nab',
               b'HTTP/2 200\r\n\r\n', b'ICY 200 OK\r\n\r\n',
               b'HTTP/1.1 200 OK\r\nX: ' + b'a' * 600 + b'\r\n\r\n']
    for raw in refused:
        check(run(exe, tmp, 'http', raw)['rc'] == -1, 'http: refused %r' % raw[:40])
    r = run(exe, tmp, 'http', b'HTTP/1.1 200 OK\r\nContent-Length: 5120\r\n\r\n' + b'x' * 5000)
    check(r['rc'] == 0 and r['body'] == 5000, 'http: still waiting for the body')
    r = run(exe, tmp, 'http', b'HTTP/1.1 200 OK\r\nContent-Length: 5120\r\n\r\n' + b'x' * 5120)
    check(r['rc'] == 1 and r['body'] == 5120, 'http: largest body')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        for t in (test_full, test_tracked, test_limits, test_types, test_body, test_body_sync, test_display, test_zones, test_source, test_http):
            t(exe, tmp)
    for f in failures:
        print('FAIL', f)
    print('%s: %d feed checks, %d failed' % ('PASS' if not failures else 'FAIL', checks, len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
