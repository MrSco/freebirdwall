"""The owned dashboard with made-up data, for screenshots and page work.

    py runtime/firmware/tools/dashboard_demo.py [--port 8812]

Serves main/dashboard.html (the page the wall serves) and answers its API the
way the board does (DASHBOARD.md, SETTINGS.md), from invented settings around
JFK: no wall, no secrets, nothing saved. Any Authorization header counts as
signed in; open http://127.0.0.1:8812/ and press "Show a code on the wall",
then type any six digits.

The display preview runs main/preview.wasm (tools/build_preview.py) with two
made-up flights; logos come from build/logo-fixtures when tools/extract_logos.py
has made them from a private backup, else the cards show none.
"""
from __future__ import annotations

import argparse
import json
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAIN = Path(__file__).resolve().parent.parent / 'main'
LOGOS = Path(__file__).resolve().parent.parent / 'build' / 'logo-fixtures'
OFFSET_MIN = -240                       # New York, summer

SETTINGS = {
    'version': 2,
    'display_config': {
        'layout_template': 'DEFAULT', 'loading_template': 'CLOCK', 'title_text': 'airline',
        'flight_display_speed': 'normal', 'max_flights_behavior': 'text', 'brightness_percent': 60,
        'border_enable': False, 'border_brightness_percent': 100, 'anchor_airports': ['KJFK'],
        'metric_display': {'alt_b_unit': 'feet', 'vel_unit': 'mph', 'vr_unit': 'fpm', 'trk_unit': 'cardinal',
                           'dist_unit': 'miles'},
        'time_zone': {'standard_utc_offset_minutes': -300, 'dst_rule': 'north_america'},
        'clock_is_24hr': False, 'clock_show_seconds': False,
        'clock': {'style': 'DEFAULT', 'show_date': True,
                  'additional_timezones': [{'label': 'London', 'time_zone': {'standard_utc_offset_minutes': 0,
                                                                              'dst_rule': 'europe'}}]},
        'date_format': {'order': 'month_day', 'show_weekday': True}, 'locale': 'en', 'display_enabled': True,
        'sleep_time': {'start': '23:00', 'end': '06:30', 'sleep_brightness_percent': 0},
    },
    'request_config': {
        'radius_requests': [{'latitude': 40.6413, 'longitude': -73.7781, 'radius_km': 15, 'name': 'JFK'},
                            {'latitude': 40.7769, 'longitude': -73.8740, 'radius_km': 8, 'name': 'LaGuardia'}],
        'data_filters': {'show_light_ac': True, 'show_heli_ac': True, 'show_commercial_ac': True,
                         'show_smalljet_ac': True, 'show_other_ac': True, 'show_na_routes': True},
        'tracked_flights': [{'flight_number': 'B6123', 'auto_remove': True, 'show_distance_travelled': True,
                             'show_metrics': True, 'created_at': '2026-10-20T12:00:00.000000+00:00'}],
    },
    'owned_config': {'flight_source': 'community', 'follow_app': True, 'scroll_speed': 'normal', 'progress_bar': True},
}
ALIASES = {'revision': 1, 'aliases': [{'kind': 'iata', 'from': 'JFK', 'to': 'Kennedy'}]}
state = {'revision': 7, 'code_until': 0.0}
FLIGHTS = {'state': 3, 'tracked': False, 'overflow': 0, 'flights': [
    {'icao': 'a8b1c2', 'cs': 'JBU1023', 'reg': 'N968JT', 'al_code': 'JBU', 'al_name': 'JetBlue Airways',
     'orig_iata': 'JFK', 'dest_iata': 'SJU', 'orig_icao': 'KJFK', 'dest_icao': 'TJSJ', 'orig': 'John F Kennedy Intl',
     'dest': 'Luis Munoz Marin Intl', 'ac_type': 'A321', 'ac_name': 'A321neo', 'alt_b': 2286, 'vel': 128, 'trk': 135,
     'vr': 9, 'prog': 3},
    {'icao': 'a3d4e5', 'cs': 'DAL1311', 'reg': 'N813DN', 'al_code': 'DAL', 'al_name': 'Delta Air Lines',
     'orig_iata': 'ATL', 'dest_iata': 'JFK', 'orig_icao': 'KATL', 'dest_icao': 'KJFK',
     'orig': 'Hartsfield-Jackson Atlanta Intl', 'dest': 'John F Kennedy Intl', 'ac_type': 'B739', 'ac_name': '737-900',
     'alt_b': 1067, 'vel': 82, 'trk': 40, 'vr': -4, 'prog': 98}]}


def stats():
    now = time.time() + OFFSET_MIN * 60
    hour, day = int(now // 3600), int(now // 86400)
    rnd = random.Random(7)
    hours = [[h, rnd.randint(4, 30) if 6 <= h % 24 <= 22 else rnd.randint(0, 3), 60] for h in range(hour - 47, hour + 1)]
    airlines = [['DAL', 48], ['JBU', 41], ['AAL', 29], ['UAL', 18], ['RPA', 11], ['EJA', 7], ['SWA', 6], ['EDV', 5]]
    types = [['A21N', 34], ['B738', 31], ['CRJ9', 26], ['E75L', 22], ['A320', 19], ['BCS3', 14]]
    orig = [['JFK', 52], ['LGA', 34], ['BOS', 19], ['ATL', 16], ['ORD', 13], ['MIA', 11]]
    dest = [['JFK', 49], ['LGA', 31], ['EWR', 21], ['MCO', 15], ['BOS', 14], ['LAX', 10]]
    days = []
    for d in range(day - 30, day + 1):
        tot = rnd.randint(150, 270)
        rk = int(tot * 0.72)
        scale = tot / 180.0
        al = [[k, max(1, int(v * scale * rnd.uniform(0.8, 1.2)))] for k, v in airlines]
        tp = [[k, max(1, int(v * scale * rnd.uniform(0.8, 1.2)))] for k, v in types]
        og = [[k, max(1, int(v * scale * rnd.uniform(0.8, 1.2)))] for k, v in orig]
        ds = [[k, max(1, int(v * scale * rnd.uniform(0.8, 1.2)))] for k, v in dest]
        days.append([d, tot, 1440, al, rk, tp, og, ds])
    days[-1][1] = sum(h[1] for h in hours if h[0] // 24 == day)
    days[-1][2] = (hour % 24 + 1) * 60
    days[-1][4] = int(days[-1][1] * 0.74)
    # Synthetic glowing corridor heatmap around JFK (40x40)
    grid = [0] * (40 * 40)
    corridors = [((4, 34), (20, 20), 65), ((36, 8), (20, 20), 58), ((18, 2), (20, 20), 42),
                 ((38, 26), (20, 20), 36), ((2, 16), (34, 32), 24), ((8, 38), (30, 4), 20)]
    for (x0, y0), (x1, y1), weight in corridors:
        for step in range(45):
            t = step / 44.0
            cx = int(round(x0 + (x1 - x0) * t))
            cy = int(round(y0 + (y1 - y0) * t))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = cx + dx, cy + dy
                    if 0 <= nx < 40 and 0 <= ny < 40:
                        w = weight if (dx == 0 and dy == 0) else weight // 3
                        grid[ny * 40 + nx] = min(230, grid[ny * 40 + nx] + w)
    cells = [[i, v] for i, v in enumerate(grid) if v]
    unix_now = int(time.time())
    rare = [
        [unix_now - 1800, 3, 'A3ST', 'BGA142', 'F-GSTA', 'BGA', 'TLS', 'JFK'],
        [unix_now - 7200, 2, 'A388', 'UAE201', 'A6-EEU', 'UAE', 'DXB', 'JFK'],
        [unix_now - 19400, 2, 'B748', 'DLH400', 'D-ABYA', 'DLH', 'FRA', 'JFK'],
        [unix_now - 41000, 1, 'B752', 'UAL1641', 'N14120', 'UAL', 'EWR', 'SFO'],
    ]
    return {
        'hour': hour, 'day': day, 'offset': OFFSET_MIN,
        'sync': {'configured': False, 'pending': False, 'ok': False, 'restored': False, 'status': 0, 'at': 0},
        'stats': {
            'hours': hours, 'days': days, 'rare': rare, 'rare_total': len(rare),
            'heat': {'lat': 40.709, 'lon': -73.826, 'dlat': 0.19, 'dlon': 0.24, 'n': 40, 'scale': 1,
                     'since': unix_now - 5 * 86400, 'pts': 4820, 'cells': cells},
        },
    }


def sync():
    t = time.time()
    iso = lambda s: time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(s))
    return {'enabled': True, 'show_upcoming': True, 'main_view': 'active', 'upcoming_view': 'next',
            'show_deadhead': True, 'show_reserve': False, 'message': 'Home for dinner',
            'legs': [{'al': 'B6', 'fn': '123', 'orig': 'JFK', 'dest': 'FLL', 'dep': iso(t + 2 * 3600),
                      'arr': iso(t + 5 * 3600), 'type': 'operating', 'status': ''},
                     {'al': 'B6', 'fn': '124', 'orig': 'FLL', 'dest': 'JFK', 'dep': iso(t + 28 * 3600),
                      'arr': iso(t + 31 * 3600), 'type': 'operating', 'status': ''}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, status, body, ctype='application/json'):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def signed_in(self):
        return bool(self.headers.get('Authorization'))

    def settings(self):
        return {'revision': state['revision'], 'settings': SETTINGS, 'aliases': ALIASES, 'feed': 3,
                'weather': {'source': 'aviationweather', 'station': 'KJFK', 'status': 200, 'requests': 4, 'failures': 0},
                'images': {'state': 'up_to_date', 'installed': '0.1.11', 'available': '0.1.11', 'error': '', 'done': 0,
                           'total': 0}}

    def do_GET(self):
        path = self.path.split('?')[0]
        if path in ('/', '/index.html'):
            return self.send(200, (MAIN / 'dashboard.html').read_bytes(), 'text/html; charset=utf-8')
        if path == '/icon.png':
            return self.send(200, (MAIN / 'icon.png').read_bytes(), 'image/png')
        if path == '/preview.wasm':
            return self.send(200, (MAIN / 'preview.wasm').read_bytes(), 'application/wasm')
        if path == '/api/session':
            left = max(0, int(state['code_until'] - time.time()))
            return self.send(200, {'signed_in': self.signed_in(), 'code_showing': left > 0, 'seconds_left': left})
        if path == '/api/games':                # the games need no sign-in, as on the wall
            return self.send(200, {'in_rotation': False, 'qr_showing': False, 'game': 'none', 'players': 0})
        if not self.signed_in():
            return self.send(401, {'error': 'sign in first'})
        answers = {
            '/api/settings': self.settings, '/api/stats': stats, '/api/sync': sync,
            '/api/tracking': lambda: {'key_set': False, 'key_end': '', 'in_use': False, 'rejected': False,
                                      'flights_calls': 0, 'position_calls': 0, 'last_status': 0},
            '/api/settings/import': lambda: {'pending': False, 'result': 'ok', 'checked': int(time.time()) - 120,
                                             'changed_at': 0, 'changed': 0, 'status': 200},
            '/api/games': lambda: {'in_rotation': False, 'qr_showing': False, 'game': 'none', 'players': 0},
            '/api/log': lambda: None,
        }
        if path == '/api/log':
            return self.send(200, b'--- boot 1, reset reason 1 ---\nFWSTATE {"board_ok":true}\n', 'text/plain')
        if path == '/api/flights':
            return self.send(200, FLIGHTS)
        if path == '/api/weather':              # what JFK might report (the wall's own record)
            return self.send(200, {'v': 1, 'stn': 'KJFK', 'name': 'New York JFK', 'cat': 'MVFR', 'cond': 'cloudy',
                                   'raw': 'KJFK 171951Z 31012KT 6SM BR BKN018 OVC035 08/04 A3002 RMK AO2',
                                   'obs': int(time.time()) - 900, 'wdir': 310, 'wspd': 12, 'vis_m': 9656,
                                   'ceil_ft': 1800, 'temp_c': 8})
        if path == '/api/memory':
            return self.send(200, {'free': 35844, 'min_free': 10984, 'largest': 14848})
        if path == '/api/logo':                 # the airline's own logo only (the wall also has category ones)
            al = ''.join(c for c in self.path.partition('al=')[2].split('&')[0] if c.isalnum())
            logo = LOGOS / (al + '.png')
            return self.send(200, logo.read_bytes(), 'image/png') if al and logo.exists() else self.send(404, {})
        if path == '/api/explore':
            return self.send(200, {
                'pending': False, 'ok': True, 'status': 200, 'at': int(time.time()),
                'lat': state.get('ex_lat', 51.47), 'lon': state.get('ex_lon', -0.45), 'dist': state.get('ex_dist', 25),
                'count': 6, 'ac': [
                    [state.get('ex_lat', 51.47) + 0.05, state.get('ex_lon', -0.45) - 0.08, 92, 'A388', 'BAW11', 'G-XLEA', 8400],
                    [state.get('ex_lat', 51.47) - 0.04, state.get('ex_lon', -0.45) + 0.06, 270, 'A35K', 'VIR45', 'G-VLUX', 12500],
                    [state.get('ex_lat', 51.47) + 0.09, state.get('ex_lon', -0.45) + 0.03, 185, 'B789', 'AAL101', 'N824AN', 16200],
                    [state.get('ex_lat', 51.47) - 0.07, state.get('ex_lon', -0.45) - 0.05, 45, 'A21N', 'EZY812', 'G-UZHA', 6800],
                    [state.get('ex_lat', 51.47) + 0.02, state.get('ex_lon', -0.45) + 0.11, 265, 'B738', 'RYR402', 'EI-DCL', 11000],
                    [state.get('ex_lat', 51.47) - 0.02, state.get('ex_lon', -0.45) - 0.02, 110, 'B748', 'CLX742', 'LX-VCJ', 21000],
                ],
            })
        if path == '/api/stats/checkpoint':
            return self.send(200, b'FWANdemo-checkpoint-bytes', 'application/octet-stream')
        if path in answers:
            return self.send(200, answers[path]())
        self.send(404, {'error': 'not here'})

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def do_POST(self):
        path, raw = self.path.split('?')[0], self.body()
        if path == '/api/pair/start':
            state['code_until'] = time.time() + 120
            return self.send(200, {'seconds': 120})
        if path == '/api/pair/finish':
            state['code_until'] = 0
            return self.send(200, {'key': 'd' * 64})
        if path == '/api/settings/import':
            return self.send(202, {'pending': False, 'result': 'ok', 'checked': int(time.time()), 'changed_at': 0,
                                   'changed': 0, 'status': 200})
        if path == '/api/games':
            return self.send(200, {'in_rotation': False, 'qr_showing': True, 'game': 'none', 'players': 0})
        if path == '/api/explore':
            j = json.loads(raw or b'{}')
            state['ex_lat'] = j.get('lat', 51.47)
            state['ex_lon'] = j.get('lon', -0.45)
            state['ex_dist'] = j.get('dist', 25)
            return self.send(202, {'pending': True})
        if path == '/api/stats/checkpoint':
            return self.send(200, {'ok': True})
        if path == '/api/stats/sync':
            return self.send(202, {'configured': True, 'pending': False, 'ok': True, 'status': 200, 'at': int(time.time())})
        self.send(200, {})

    def do_PUT(self):
        path, body = self.path.split('?')[0], self.body()
        if path in ('/api/settings', '/api/aliases'):
            state['revision'] += 1          # accepted, not kept: a demo
            return self.send(200, self.settings())
        if path == '/api/sync':
            return self.send(200, sync())
        self.send(200, json.loads(body or b'{}'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8812)
    a = ap.parse_args()
    print('dashboard demo on http://127.0.0.1:%d/' % a.port)
    ThreadingHTTPServer(('127.0.0.1', a.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
