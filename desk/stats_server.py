"""Optional LAN/NAS persistence server for FreeBirdWall analytics checkpoints.

Run on any Raspberry Pi, NAS, Docker host, or always-on computer on your Wi-Fi:

    py desk/stats_server.py --port 8090 --dir ./stats-data

Then set your wall's Backup Server URL in the dashboard (Stats -> Backup) to:

    http://<this-machine-ip>:8090

Endpoints:
  GET  /checkpoint   -> returns the latest saved binary .fwan checkpoint (404 if none)
  PUT  /checkpoint   -> validates ("FWAN" magic + size) and atomically saves it to disk
  GET  /health       -> JSON status (bytes, updated timestamp, backups count)
"""
from __future__ import annotations

import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAGIC = b'FWAN'
MAX_BYTES = 16384


def make_handler(data_dir: Path):
    ckpt_path = data_dir / 'checkpoint.fwan'
    meta_path = data_dir / 'meta.json'
    state = {'backups': 0, 'updated': 0}
    if meta_path.exists():
        try:
            state.update(json.loads(meta_path.read_text(encoding='utf-8')))
        except Exception:
            pass

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            print('[stats_server]', self.address_string(), fmt % args)

        def send_cors(self):
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Methods', 'GET, PUT, OPTIONS')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')

        def send_bytes(self, code: int, body: bytes, ctype: str):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_cors()
            self.end_headers()
            self.wfile.write(body)

        def send_json(self, code: int, obj: dict):
            self.send_bytes(code, json.dumps(obj).encode('utf-8'), 'application/json')

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_cors()
            self.end_headers()

        def do_GET(self):
            path = self.path.split('?')[0].rstrip('/') or '/'
            if path in ('/', '/health'):
                size = ckpt_path.stat().st_size if ckpt_path.exists() else 0
                return self.send_json(200, {
                    'ok': True,
                    'has_checkpoint': ckpt_path.exists(),
                    'bytes': size,
                    'updated': state['updated'],
                    'backups': state['backups'],
                })
            if path.endswith('/checkpoint'):
                if not ckpt_path.exists():
                    return self.send_json(404, {'error': 'no checkpoint saved yet'})
                return self.send_bytes(200, ckpt_path.read_bytes(), 'application/octet-stream')
            return self.send_json(404, {'error': 'not found'})

        def do_PUT(self):
            path = self.path.split('?')[0].rstrip('/')
            if not path.endswith('/checkpoint'):
                return self.send_json(404, {'error': 'not found'})
            try:
                length = int(self.headers.get('Content-Length') or '0')
            except ValueError:
                return self.send_json(400, {'error': 'bad content-length'})
            if length < 16 or length > MAX_BYTES:
                return self.send_json(400, {'error': 'invalid checkpoint size'})
            blob = self.rfile.read(length)
            if len(blob) != length or not blob.startswith(MAGIC):
                return self.send_json(400, {'error': 'invalid checkpoint header'})
            data_dir.mkdir(parents=True, exist_ok=True)
            tmp = data_dir / 'checkpoint.fwan.tmp'
            tmp.write_bytes(blob)
            os.replace(tmp, ckpt_path)
            state['backups'] = int(state.get('backups', 0)) + 1
            state['updated'] = int(time.time())
            meta_tmp = data_dir / 'meta.json.tmp'
            meta_tmp.write_text(json.dumps(state), encoding='utf-8')
            os.replace(meta_tmp, meta_path)
            return self.send_json(200, {'ok': True, 'bytes': len(blob), 'updated': state['updated']})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='0.0.0.0', help='Bind address (default 0.0.0.0)')
    ap.add_argument('--port', type=int, default=8090, help='Port to listen on (default 8090)')
    ap.add_argument('--dir', default='./freebirdwall-stats', help='Directory for checkpoint files')
    args = ap.parse_args()
    data_dir = Path(args.dir).resolve()
    data_dir.mkdir(parents=True, exist_ok=True)
    srv = ThreadingHTTPServer((args.host, args.port), make_handler(data_dir))
    print(f'FreeBirdWall stats backup server on http://{args.host}:{args.port}/ (storing in {data_dir})')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
