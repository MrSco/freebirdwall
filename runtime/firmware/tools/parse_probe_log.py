"""Summarise a console capture from the owned-firmware probe. No device access.

    py tools/parse_probe_log.py capture.log [--json out.json]

Extracts FWSTATE/FWMEM lines and answers the questions the probe exists for:
how much internal heap is free once Wi-Fi is up, the lowest point reached
during TLS handshakes, the largest contiguous block at that moment, the probe
task's worst stack margin, and whether heap returns to the same level after
each request (a per-connection leak would show as a downward trend).
"""
import argparse
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r'\b(FWMEM|FWSTATE) (\{.*\})\s*$')


def parse(text):
    mem, state = [], []
    for raw in text.splitlines():
        m = LINE.search(raw)
        if not m:
            continue
        try:
            row = json.loads(m.group(2))
        except json.JSONDecodeError:
            continue
        (mem if m.group(1) == 'FWMEM' else state).append(row)
    return mem, state


def summarise(mem, state):
    by_stage = {}
    for row in mem:
        by_stage.setdefault(row['stage'], []).append(row)

    def first(stage, key):
        rows = by_stage.get(stage)
        return rows[0].get(key) if rows else None

    rounds = by_stage.get('route_done', [])
    handshakes = by_stage.get('route_handshake', [])
    probe_rows = [r for r in mem if r.get('task') == 'fw_probe']
    after = [r['free'] for r in rounds]
    return {
        'boot_free': first('boot', 'free'),
        'boot_largest': first('boot', 'largest'),
        'tls_arena': first('boot', 'tls_arena'),
        'after_wifi_ip_free': first('wifi_got_ip', 'free'),
        'after_wifi_ip_largest': first('wifi_got_ip', 'largest'),
        'handshake_min_free': min((r['free'] for r in handshakes), default=None),
        'handshake_min_largest': min((r['largest'] for r in handshakes), default=None),
        'lowest_min_free_since_boot': min((r['min_free'] for r in mem), default=None),
        'probe_task_min_stack_free': min((r['stack_free'] for r in probe_rows), default=None),
        'rounds': len(rounds),
        'rounds_ok': sum(1 for r in rounds if r.get('ok')),
        'tls_peak_payload_max': max((r.get('peak_payload', 0) for r in rounds), default=None),
        'free_after_each_round': after,
        'per_round_drift': (after[-1] - after[0]) if len(after) > 1 else None,
        'remaining_blocks_all_zero': all(r.get('remaining_blocks') == 0 for r in rounds) if rounds else None,
        'final': by_stage.get('final', [{}])[-1],
        'state': state,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', type=Path)
    ap.add_argument('--json', type=Path)
    a = ap.parse_args()
    mem, state = parse(a.log.read_text(encoding='utf-8', errors='replace'))
    if not mem:
        sys.exit('no FWMEM lines found')
    s = summarise(mem, state)
    for k, v in s.items():
        if k not in ('state', 'final'):
            print('%-28s %s' % (k, v))
    if a.json:
        a.json.write_text(json.dumps(s, indent=2))


if __name__ == '__main__':
    main()
