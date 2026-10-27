"""The dashboard's display preview draws exactly what the desktop build of the
same screen code draws.

    py runtime/firmware/tests/test_preview.py [--zig <zig.exe>] [--keep <dir>]

Builds preview/fw_preview.c twice, to WebAssembly (tools/build_preview.py, the
committed main/preview.wasm must match a fresh build; without Zig the committed
one is used and that check is skipped) and with the desktop gcc
(tests/preview_host.c), runs the same scenarios through both (Node runs
the wasm with the dashboard's loader, tests/preview_run.mjs) and compares
every frame byte for byte. The scenarios cover the layouts, title text,
units, the border, the loading templates, the clock, aliases, the "too many"
screen, tracked flights, scrolling, and logos when build/logo-fixtures has
them (tools/extract_logos.py). --keep writes LED-style PNGs of every frame.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
sys.path.insert(0, str(FW / 'tools'))
import build_preview  # noqa: E402

FIXTURES = FW / 'build' / 'logo-fixtures'
NOW = 1793050000.0          # a fixed unix time (2026-10-26, a Monday evening in Florida)


def flight(cs, al, name, o, d, on, dn, ac, **kw):
    f = {'icao': cs.lower()[:6], 'cs': cs, 'al_code': al, 'al_name': name, 'orig_iata': o, 'dest_iata': d,
         'orig': on, 'dest': dn, 'ac_name': ac, 'alt_b': 10668, 'vel': 231.5, 'trk': 172, 'vr': 0}
    f.update(kw)
    return f


FLIGHTS = [flight('JBU615', 'JBU', 'JetBlue Airways', 'BOS', 'FLL', 'Boston Logan Intl', 'Fort Lauderdale Hollywood Intl',
                  'A320', reg='N594JB'),
           flight('DAL402', 'DAL', 'Delta Air Lines', 'ATL', 'PBI', 'Hartsfield-Jackson Atlanta Intl', 'Palm Beach Intl',
                  'A321', reg='N392DN'),
           flight('AAL1234', 'AAL', 'American Airlines', 'CLT', 'MIA', 'Charlotte Douglas Intl', 'Miami Intl',
                  '737-800', reg='N921NN')]
TRACKED = flight('AAL1234', 'AAL', 'American Airlines', 'CLT', 'PBI', 'Charlotte Douglas', 'Palm Beach Intl',
                 '737-800', has_trk=True, prog=42, dist=482803, dep='35m ago', arr='1h 10m', over='South Carolina',
                 trk_metrics=True)


def settings(**dc):
    owned = dc.pop('owned', {})
    base = {'title_text': 'airline', 'layout_template': 'DEFAULT', 'flight_display_speed': 'normal',
            'metric_display': {'alt_b_unit': 'feet', 'vel_unit': 'mph', 'vr_unit': 'fpm', 'trk_unit': 'cardinal',
                               'dist_unit': 'miles'},
            'time_zone': {'standard_utc_offset_minutes': -300, 'dst_rule': 'north_america'},
            'clock': {'style': 'LARGE', 'show_date': True}, 'date_format': {'order': 'month_day', 'show_weekday': True}}
    base.update(dc)
    return {'settings': {'display_config': base, 'owned_config': owned}}


def run_steps(cmds, start, until, every=40):
    for t in range(start, until, every):
        cmds.append({'step': [t, NOW + t / 1000]})


def scenarios():
    logos = []
    for code in ('JBU', 'DAL', 'AAL'):
        p = FIXTURES / (code + '.png')
        logos.append({'logo': [code, '', str(p) if p.exists() else '']})
    out = {}

    def scene(name, *setup, until=12000, dumps=(0, 3100, 6500, 11900)):
        cmds = list(setup)
        t = 0
        for i, d in enumerate(dumps):
            run_steps(cmds, t, d + 1)
            t = d + 1
            cmds.append({'dump': '%s_%d' % (name, i)})
        out[name] = cmds

    fl = {'flights': {'flights': FLIGHTS, 'overflow': 0}}
    scene('default', settings(), fl, *logos)
    scene('no_logo_callsign', settings(layout_template='NO_LOGO', title_text='callsign'), fl, *logos)
    scene('technical_metric', settings(layout_template='TECHNICAL', title_text='registration',
                                       metric_display={'alt_b_unit': 'meters', 'vel_unit': 'kmh', 'vr_unit': 'mps',
                                                       'trk_unit': 'degrees', 'dist_unit': 'kilometers'}), fl, *logos)
    scene('border_fast_scroll', settings(border_enable=True, border_brightness_percent=40, flight_display_speed='fast',
                                         owned={'scroll_speed': 'fast'}), fl, *logos)
    scene('too_many', settings(max_flights_behavior='text', flight_display_speed='fast'),
          {'flights': {'flights': FLIGHTS, 'overflow': 9}}, *logos, dumps=(0, 2100, 4200, 6300, 8400))
    scene('indicator', settings(max_flights_behavior='indicator_led'), {'flights': {'flights': FLIGHTS, 'overflow': 4}},
          *logos)
    scene('aliases', settings(), fl, {'aliases': [{'kind': 'iata', 'from': 'FLL', 'to': 'LAU'},
                                                  {'kind': 'name', 'from': 'Palm Beach Intl', 'to': 'Home'}]}, *logos)
    scene('anchors', settings(anchor_airports=['FLL', 'PBI']), fl, *logos)
    scene('tracked', settings(), {'flights': {'flights': [TRACKED], 'tracked': True}}, *logos,
          until=25000, dumps=(0, 6000, 12000, 18000))
    for loading in ('TEXT', 'ELLIPSIS', 'BLANK', 'CLOCK'):
        scene('empty_' + loading.lower(), settings(loading_template=loading), {'flights': {'flights': []}},
              dumps=(0, 1500))
    scene('clock_small_24h_extra', settings(clock={'style': 'SMALL', 'show_date': False, 'additional_timezones': [
        {'label': 'London', 'time_zone': {'standard_utc_offset_minutes': 0, 'dst_rule': 'europe'}}]},
        clock_is_24hr=True, clock_show_seconds=True), {'mode': 2}, dumps=(0, 2000))
    scene('clock_large_dayfirst', settings(date_format={'order': 'day_month', 'show_weekday': False}), {'mode': 2},
          dumps=(0,))
    # a setting changed in the middle: the preview starts the card over, like the wall
    mid = [settings(), fl] + logos
    run_steps(mid, 0, 2001)
    mid += [{'dump': 'change_0'}, settings(layout_template='NO_LOGO')]
    run_steps(mid, 2001, 4001)
    mid.append({'dump': 'change_1'})
    out['change'] = mid
    # a new list from the wall mid-rotation: the rotation carries on, as on the wall
    fresh = [settings(flight_display_speed='fast'), fl] + logos
    run_steps(fresh, 0, 2501)
    fresh += [{'dump': 'refresh_0'}, {'flights': {'flights': FLIGHTS[1:] + [TRACKED | {'has_trk': False}], 'overflow': 2}}]
    run_steps(fresh, 2501, 9001)
    fresh.append({'dump': 'refresh_1'})
    out['refresh'] = fresh
    # the progress bar (owned_config.progress_bar): on the bottom row, only when on and known
    prog = {'flights': {'flights': [dict(f, prog=p) for f, p in zip(FLIGHTS, (38, 100, float('nan')))], 'overflow': 0}}
    prog['flights']['flights'][2].pop('prog')
    scene('progress', settings(owned={'progress_bar': True}, flight_display_speed='fast'), prog, *logos,
          until=30000, dumps=tuple(range(0, 30000, 1500)))
    scene('progress_off', settings(flight_display_speed='fast'), prog, *logos, dumps=(0,))
    return out


def led_png(rgba, path):
    from PIL import Image, ImageDraw
    pitch = 6
    img = Image.new('RGB', (128 * pitch, 64 * pitch), (18, 18, 20))
    d = ImageDraw.Draw(img)
    for y in range(64):
        for x in range(128):
            i = (y * 128 + x) * 4
            c = tuple(rgba[i:i + 3])
            d.ellipse([x * pitch + 1, y * pitch + 1, x * pitch + pitch - 2, y * pitch + pitch - 2],
                      fill=c if any(c) else (38, 38, 42))
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zig')
    ap.add_argument('--keep', type=Path)
    a = ap.parse_args()
    node = shutil.which('node') or sys.exit('Node.js is required')
    checks = 0
    bars = set()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        if build_preview.find_zig(a.zig):
            fresh = build_preview.build_wasm(tmp / 'preview.wasm', a.zig)
            if fresh.read_bytes() != build_preview.OUT.read_bytes():
                sys.exit('FAIL: main/preview.wasm is not a fresh build; run tools/build_preview.py')
            checks += 1
        else:
            fresh = build_preview.OUT
            print('no Zig: checking the committed main/preview.wasm only')
        host = build_preview.build_host(HERE / 'preview_host.c', tmp / 'preview_host.exe')
        for name, cmds in scenarios().items():
            scen = tmp / (name + '.json')
            scen.write_text(json.dumps(cmds), encoding='utf-8')
            (tmp / 'h').mkdir(exist_ok=True)
            (tmp / 'w').mkdir(exist_ok=True)
            h = subprocess.run([str(host), str(scen), str(tmp / 'h')], capture_output=True, text=True)
            w = subprocess.run([node, str(HERE / 'preview_run.mjs'), str(fresh), str(scen), str(tmp / 'w')],
                               capture_output=True, text=True)
            if h.returncode or w.returncode:
                sys.exit('FAIL %s: %s%s' % (name, h.stderr, w.stderr))
            if h.stdout != w.stdout:
                sys.exit('FAIL %s: reports differ\n desktop: %s\n wasm:    %s' % (name, h.stdout, w.stdout))
            for line in h.stdout.splitlines():
                dump = line.split()[0]
                hb, wb = (tmp / 'h' / (dump + '.rgba')).read_bytes(), (tmp / 'w' / (dump + '.rgba')).read_bytes()
                if hb != wb:
                    diff = sum(1 for i in range(0, len(hb), 4) if hb[i:i + 3] != wb[i:i + 3])
                    sys.exit('FAIL %s: %d pixels differ' % (dump, diff))
                if not any(hb[i] or hb[i + 1] or hb[i + 2] for i in range(0, len(hb), 4)) and 'blank' not in dump:
                    sys.exit('FAIL %s: nothing drawn' % dump)
                if dump.startswith('progress'):
                    row = [tuple(hb[(62 * 128 + x) * 4:(62 * 128 + x) * 4 + 3]) for x in range(128)]
                    green = sum(1 for c in row if c == (0, 255, 0))
                    grey = sum(1 for c in row if c == (66, 65, 66))
                    if (green or grey) and green + grey != 124 or dump.startswith('progress_off') and (green or grey):
                        sys.exit('FAIL %s: bottom row %d green, %d grey' % (dump, green, grey))
                    bars.add(green if green or grey else None)
                    checks += 1
                checks += 1
                if a.keep:
                    a.keep.mkdir(parents=True, exist_ok=True)
                    led_png(hb, a.keep / (dump + '.png'))
    if bars != {47, 124, None}:                     # 38 % and 100 % of 124 pixels, and a flight with no bar
        sys.exit('FAIL progress: bars seen %r' % sorted(bars, key=str))
    print('PASS: %d preview checks (WebAssembly and desktop frames identical%s)' % (
        checks, '' if (FIXTURES / 'AAL.png').exists() else '; no logo fixtures, logos left out'))


if __name__ == '__main__':
    main()
