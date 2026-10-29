"""Desktop tests for components/fw_settings (owned settings edits).

    py tests/test_settings.py
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
CJSON = Path.home() / 'esp' / 'esp-idf-v4.4.7' / 'components' / 'json' / 'cJSON'
checks, failures = 0, []


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)


def build(tmp):
    gcc = shutil.which('gcc') or sys.exit('a desktop gcc is required')
    exe = Path(tmp) / 'settings_dump.exe'
    obj = Path(tmp) / 'cjson.o'
    r = subprocess.run([gcc, '-std=c11', '-O1', '-w', '-c', str(CJSON / 'cJSON.c'), '-o', str(obj)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('cJSON build failed:\n' + r.stderr[-3000:])
    r = subprocess.run([gcc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I%s' % (FW / 'components/fw_settings/include'), '-I%s' % CJSON,
                        str(HERE / 'settings_dump.c'), str(FW / 'components/fw_settings/fw_settings.c'), str(obj),
                        '-o', str(exe), '-lm'], capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stderr[-3000:])
    return exe


CURRENT = {
    'version': 2,
    'request_config': {'radius_request': {'latitude': 1, 'longitude': 2}, 'type': 'radius',
                       'geo_requests': [{'coordinates': [[1, 2], [3, 4], [5, 6]]}],
                       'tracked_flights': [{'flight_number': 'AA100', 'created_at': '2026-10-01T00:00:00Z',
                                            'auto_remove': True}]},
    'display_config': {'layout_template': 'DEFAULT', 'brightness_percent': 15, 'model': 'mini-v1'},
    'plus': {'sync': {'enabled': True}},
}


def apply(exe, tmp, edit, current=None):
    a, b = Path(tmp) / 'cur.json', Path(tmp) / 'edit.json'
    if current is None:
        current = CURRENT
    a.write_text(current if isinstance(current, str) else json.dumps(current))
    b.write_text(edit if isinstance(edit, str) else json.dumps(edit))
    r = subprocess.run([str(exe), str(a), str(b)], capture_output=True, text=True)
    return json.loads(r.stdout)


def ok(exe, tmp, edit, what, current=None):
    r = apply(exe, tmp, edit, current)
    check(r['rc'] == 0, '%s: refused %s %s' % (what, r['field'], r['why']))
    return r['doc'] or {}


def bad(exe, tmp, edit, field, what):
    r = apply(exe, tmp, edit)
    check(r['rc'] == -1 and r['doc'] is None and r['field'] == field, '%s: %r' % (what, r))


def test_display(exe, tmp):
    full = {'layout_template': 'NO_LOGO', 'loading_template': 'CLOCK', 'title_text': 'callsign',
            'flight_display_speed': 'fast', 'max_flights_behavior': 'indicator_led',
            'metric_display': {'alt_b_unit': 'feet', 'vel_unit': 'knots', 'vr_unit': 'fpm', 'trk_unit': 'degrees',
                               'dist_unit': 'nautical_miles'},
            'border_enable': True, 'border_brightness_percent': 40, 'brightness_percent': 60,
            'anchor_airports': ['KCLT', 'PBI'],
            'time_zone': {'standard_utc_offset_minutes': -300, 'dst_rule': 'north_america'},
            'clock_is_24hr': True, 'clock_show_seconds': False,
            'clock': {'style': 'LARGE', 'show_date': True,
                      'additional_timezones': [{'label': 'London', 'time_zone': {'standard_utc_offset_minutes': 0,
                                                                                  'dst_rule': 'europe'}}]},
            'date_format': {'order': 'day_month', 'show_weekday': True}, 'locale': 'fr', 'display_enabled': True,
            'sleep_time': {'start': '23:00', 'end': '06:30', 'sleep_brightness_percent': 0}}
    d = ok(exe, tmp, {'display_config': full}, 'display: every setting')
    for k, v in full.items():
        check(d['display_config'][k] == v, 'display: %s stored' % k)
    check(d['display_config']['model'] == 'mini-v1', 'display: untouched keys kept')
    check(d['plus'] == CURRENT['plus'] and d['version'] == 2, 'display: other sections kept')
    check(d['request_config'] == CURRENT['request_config'], 'display: request untouched')
    d = ok(exe, tmp, {'display_config': {'brightness_percent': 80}}, 'display: one setting')
    check(d['display_config'] == dict(CURRENT['display_config'], brightness_percent=80), 'display: only that changed')
    cases = [
        ({'layout_template': 'HUGE'}, 'display_config.layout_template'),
        ({'layout_template': 'PLUS_SYNC_ACTIVE_MAIN'}, 'display_config.layout_template'),
        ({'title_text': 1}, 'display_config.title_text'),
        ({'brightness_percent': 0}, 'display_config.brightness_percent'),
        ({'brightness_percent': 50.5}, 'display_config.brightness_percent'),
        ({'border_brightness_percent': 101}, 'display_config.border_brightness_percent'),
        ({'border_enable': 'yes'}, 'display_config.border_enable'),
        ({'metric_display': {'vel_unit': 'furlongs'}}, 'display_config.metric_display.vel_unit'),
        ({'metric_display': {'speed': 'kmh'}}, 'display_config.metric_display.speed'),
        ({'metric_display': 'kmh'}, 'display_config.metric_display'),
        ({'time_zone': {'standard_utc_offset_minutes': -301}}, 'display_config.time_zone.standard_utc_offset_minutes'),
        ({'time_zone': {'standard_utc_offset_minutes': 900}}, 'display_config.time_zone.standard_utc_offset_minutes'),
        ({'time_zone': {'dst_rule': 'mars'}}, 'display_config.time_zone.dst_rule'),
        ({'clock': {'additional_timezones': [{}, {}, {}]}}, 'display_config.clock.additional_timezones'),
        ({'clock': {'additional_timezones': [{'label': 'A very long label here'}]}},
         'display_config.clock.additional_timezones.label'),
        ({'clock': {'additional_timezones': [{'label': 'X', 'standard_utc_offset_minutes': 0}]}},
         'display_config.clock.additional_timezones.standard_utc_offset_minutes'),
        ({'clock': {'additional_timezones': [{'time_zone': {'standard_utc_offset_minutes': 7}}]}},
         'display_config.clock.additional_timezones.time_zone.standard_utc_offset_minutes'),
        ({'clock': {'additional_timezones': [{'time_zone': {'dst_rule': 'mars'}}]}},
         'display_config.clock.additional_timezones.time_zone.dst_rule'),
        ({'locale': 'xx'}, 'display_config.locale'),
        ({'anchor_airports': ['kclt']}, 'display_config.anchor_airports'),
        ({'anchor_airports': ['AB']}, 'display_config.anchor_airports'),
        ({'anchor_airports': ['AAA'] * 9}, 'display_config.anchor_airports'),
        ({'sleep_time': {'start': '24:00'}}, 'display_config.sleep_time.start'),
        ({'sleep_time': {'end': '7:00'}}, 'display_config.sleep_time.end'),
        ({'model': 'widescreen-v1'}, 'display_config.model'),
    ]
    for edit, field in cases:
        bad(exe, tmp, {'display_config': edit}, field, 'display: %r' % edit)


def test_request(exe, tmp):
    zones = [{'latitude': 26.7, 'longitude': -80.05, 'radius_km': 30, 'name': 'Home', 'max_altitude': 12000},
             {'latitude': 26.5, 'longitude': -80.2, 'radius_km': 10, 'min_altitude': 100}]
    d = ok(exe, tmp, {'request_config': {'radius_requests': zones}}, 'request: zones')
    r = d['request_config']
    check(r['radius_requests'] == zones, 'request: zones stored')
    check('radius_request' not in r and 'type' not in r, 'request: singular and legacy forms dropped')
    check(r['geo_requests'] == CURRENT['request_config']['geo_requests'], 'request: geo zones kept')
    tracked = [{'flight_number': 'AA100', 'auto_remove': False}, {'flight_number': 'DL42', 'show_metrics': True}]
    d = ok(exe, tmp, {'request_config': {'tracked_flights': tracked}}, 'request: tracked')
    t = d['request_config']['tracked_flights']
    check(t[0]['created_at'] == '2026-10-01T00:00:00Z' and t[0]['auto_remove'] is False, 'request: kept created_at')
    check(t[1]['created_at'] == '2026-10-19T21:40:00Z', 'request: new flight stamped now')
    f = {'show_light_ac': False, 'show_heli_ac': True, 'min_speed_mps': 50, 'airline_icao_filter': ['DAL', 'AAL']}
    d = ok(exe, tmp, {'request_config': {'data_filters': f}}, 'request: filters')
    check(d['request_config']['data_filters'] == f, 'request: filters stored')
    d = ok(exe, tmp, {'request_config': {'tracked_flights': []}}, 'request: no tracked flights')
    check(d['request_config']['tracked_flights'] == [], 'request: tracked cleared')
    cases = [
        ({'radius_requests': []}, 'request_config.radius_requests'),
        ({'radius_requests': [{'latitude': 1}] * 6}, 'request_config.radius_requests'),
        ({'radius_requests': [{'latitude': 91, 'longitude': 0}]}, 'request_config.radius_requests.latitude'),
        ({'radius_requests': [{'latitude': 1, 'longitude': 181}]}, 'request_config.radius_requests.longitude'),
        ({'radius_requests': [{'latitude': 1, 'longitude': 1, 'radius_km': 0}]},
         'request_config.radius_requests.radius_km'),
        ({'radius_requests': [{'latitude': 1, 'longitude': 1, 'radius_km': 300}]},
         'request_config.radius_requests.radius_km'),
        ({'radius_requests': [{'latitude': 1}]}, 'request_config.radius_requests'),
        ({'radius_requests': [{'latitude': 1, 'longitude': 1, 'min_altitude': 500, 'max_altitude': 100}]},
         'request_config.radius_requests'),
        ({'radius_requests': [{'latitude': 1, 'longitude': 1, 'id': 'x'}]}, 'request_config.radius_requests.id'),
        ({'data_filters': {'airline_icao_filter': ['dal']}}, 'request_config.data_filters.airline_icao_filter'),
        ({'data_filters': {'min_speed_mps': -1}}, 'request_config.data_filters.min_speed_mps'),
        ({'data_filters': {'show_ufo': True}}, 'request_config.data_filters.show_ufo'),
        ({'tracked_flights': [{'flight_number': 'aa1'}]}, 'request_config.tracked_flights.flight_number'),
        ({'tracked_flights': [{'auto_remove': True}]}, 'request_config.tracked_flights'),
        ({'tracked_flights': [{'flight_number': 'AA1', 'created_at': 'x'}]},
         'request_config.tracked_flights.created_at'),
        ({'tracked_flights': [{'flight_number': 'AA1'}] * 9}, 'request_config.tracked_flights'),
        ({'geo_requests': []}, 'request_config.geo_requests'),
    ]
    for edit, field in cases:
        bad(exe, tmp, {'request_config': edit}, field, 'request: %r' % edit)


def test_owned(exe, tmp):
    d = ok(exe, tmp, {'owned_config': {'flight_source': 'community'}}, 'owned: community')
    check(d.get('owned_config') == {'flight_source': 'community'} and d['display_config'] == CURRENT['display_config'],
          'owned: section added, others kept %r' % d)
    cur = dict(CURRENT, owned_config={'flight_source': 'community'})
    d = ok(exe, tmp, {'owned_config': {'flight_source': None}}, 'owned: null', current=cur)
    check(d.get('owned_config') == {}, 'owned: null removes the source %r' % d)
    d = ok(exe, tmp, {'owned_config': {'flight_source': 'vendor'}, 'display_config': {'brightness_percent': 9}},
           'owned: with another section')
    check(d['owned_config']['flight_source'] == 'vendor' and d['display_config']['brightness_percent'] == 9,
          'owned: both sections applied')
    bad(exe, tmp, {'owned_config': {'flight_source': 'adsbx'}}, 'owned_config.flight_source', 'owned: unknown source')
    d = ok(exe, tmp, {'owned_config': {'follow_app': False}}, 'owned: stop following the app', current=cur)
    check(d.get('owned_config') == {'flight_source': 'community', 'follow_app': False}, 'owned: follow_app kept beside the source %r' % d)
    bad(exe, tmp, {'owned_config': {'follow_app': 'no'}}, 'owned_config.follow_app', 'owned: follow_app must be a bool')
    d = ok(exe, tmp, {'owned_config': {'progress_bar': True}}, 'owned: progress bar on', current=cur)
    check(d.get('owned_config', {}).get('progress_bar') is True, 'owned: progress_bar kept %r' % d)
    bad(exe, tmp, {'owned_config': {'progress_bar': 1}}, 'owned_config.progress_bar', 'owned: progress_bar must be a bool')
    d = ok(exe, tmp, {'owned_config': {'tracking_source': 'aeroapi'}}, 'owned: tracked flight times from FlightAware', current=cur)
    check(d['owned_config'].get('tracking_source') == 'aeroapi', 'owned: tracking_source saved %r' % d)
    bad(exe, tmp, {'owned_config': {'tracking_source': 'airlabs'}}, 'owned_config.tracking_source', 'owned: unknown tracking source')
    d = ok(exe, tmp, {'owned_config': {'scroll_speed': 'slow'}}, 'owned: scroll speed', current=cur)
    check(d['owned_config'].get('scroll_speed') == 'slow', 'owned: scroll_speed saved %r' % d)
    bad(exe, tmp, {'owned_config': {'scroll_speed': 3}}, 'owned_config.scroll_speed', 'owned: scroll speed is a word')
    for enabled in (False, True, None):
        d = ok(exe, tmp, {'owned_config': {'heartbeat_enabled': enabled}}, 'owned: heartbeat', current=cur)
        check(d['owned_config'].get('heartbeat_enabled') == enabled, 'owned: heartbeat saved/reset %r' % d)
        check(d['owned_config']['flight_source'] == 'community', 'owned: heartbeat keeps the other settings')
    bad(exe, tmp, {'owned_config': {'heartbeat_enabled': 'false'}}, 'owned_config.heartbeat_enabled', 'owned: heartbeat is a bool')
    bad(exe, tmp, {'owned_config': {'colour': 1}}, 'owned_config.colour', 'owned: unknown key')
    bad(exe, tmp, {'owned_config': []}, 'owned_config', 'owned: not an object')


def test_envelope(exe, tmp):
    bad(exe, tmp, {'plus': {}}, 'plus', 'envelope: other sections refused')
    bad(exe, tmp, '[1]', '', 'envelope: not an object')
    bad(exe, tmp, 'nope', '', 'envelope: not JSON')
    r = apply(exe, tmp, {'display_config': {}}, current='not json')
    check(r['rc'] == -3, 'envelope: bad stored document')
    d = ok(exe, tmp, {}, 'envelope: empty edit')
    check(d == CURRENT, 'envelope: empty edit changes nothing')
    r = apply(exe, tmp, {'display_config': {'brightness_percent': 50}}, current={'version': 2})
    check(r['rc'] == 0 and r['doc']['display_config'] == {'brightness_percent': 50}, 'envelope: section created')
    cur = dict(CURRENT, display_config=dict(CURRENT['display_config'], sleep_time={'start': '23:00', 'end': '07:00'}))
    d = ok(exe, tmp, {'display_config': {'sleep_time': None, 'brightness_percent': 40}}, 'envelope: null removes', current=cur)
    check(d and 'sleep_time' not in d['display_config'] and d['display_config']['brightness_percent'] == 40 and
          d['display_config']['model'] == 'mini-v1', 'envelope: null removes only that key %r' % d)
    d = ok(exe, tmp, {'request_config': {'tracked_flights': None}}, 'envelope: null removes tracked')
    check(d and 'tracked_flights' not in d['request_config'] and 'radius_request' in d['request_config'],
          'envelope: null removes only tracked %r' % d)
    d = ok(exe, tmp, {'display_config': {'locale': None}}, 'envelope: null for a key not stored')
    check(d == CURRENT, 'envelope: null for a missing key changes nothing')
    bad(exe, tmp, {'request_config': {'radius_requests': None}}, 'request_config.radius_requests',
        'envelope: areas cannot be removed')
    bad(exe, tmp, {'display_config': {'clock': {'style': None}}}, 'display_config.clock.style',
        'envelope: null only at the top of a section')
    bad(exe, tmp, {'display_config': {'colour': None}}, 'display_config.colour', 'envelope: null for an unknown key')
    big = dict(CURRENT, plus={'pad': 'x' * 4000})
    r = apply(exe, tmp, {'display_config': {'brightness_percent': 50}}, current=big)
    check(r['rc'] == -2 and r['doc'] is None, 'envelope: over 4 KiB refused')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        for t in (test_display, test_request, test_owned, test_envelope):
            t(exe, tmp)
    for f in failures:
        print('FAIL', f)
    print('%s: %d settings checks, %d failed' % ('PASS' if not failures else 'FAIL', checks, len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
