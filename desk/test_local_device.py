"""LAN client and desk boundaries against an isolated mock device. No saved keys."""
import copy
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import io

import control
import local_device

TOKEN = 'a1' * 32
CONNECTION = {'ip': '192.168.50.20', 'token': TOKEN}
ROWS = [{'kind': 'iata', 'from': 'DJT', 'to': 'PBI'}]
REAL_HTTP = http.client.HTTPConnection


class MockDevice(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def response(self, code, body):
        encoded = json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Length', str(len(encoded)))
        self.send_header('Content-Type', 'application/json')
        self.send_header('ETag', self.server.etag or '"%d"' % self.server.revision)
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        self.server.requests.append((self.command, self.path, dict(self.headers)))
        if self.server.error:
            self.response(self.server.error, {'error': TOKEN + ' secret reflected'})
            return
        if self.headers.get('Authorization') != 'Bearer ' + TOKEN:
            self.response(401, {})
            return
        body = {'api_version': 1, 'schema_version': 1, 'revision': self.server.revision}
        if self.path == '/v1/aliases':
            body['aliases'] = self.server.rows
        elif self.path == '/v1/status':
            body.update(alias_count=len(self.server.rows), storage='nvs', storage_ready=True)
        elif self.path == '/v1/settings':
            self.response(200, self.server.settings)
            return
        else:
            self.response(404, {})
            return
        self.response(200, body)

    def do_PUT(self):
        self.server.requests.append((self.command, self.path, dict(self.headers)))
        raw = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.server.put_error:
            self.response(self.server.put_error, {'error': TOKEN})
            return
        if self.path == '/v1/settings':
            if self.headers.get('Origin') != f'http://{CONNECTION["ip"]}:8787':
                self.response(403, {})
                return
            self.server.settings = copy.deepcopy(raw)
            self.response(200, self.server.settings)
            return
        command = raw['owner_aliases']
        if self.headers.get('If-Match') != '"%d"' % self.server.revision:
            self.response(409, {})
            return
        if command['revision'] != self.server.revision + 1:
            self.response(422, {})
            return
        self.server.revision = command['revision']
        self.server.rows = copy.deepcopy(command['aliases'])
        if self.server.bad_ack:
            command['aliases'] = []
        self.response(200, {'api_version': 1, **command})


class LocalDeviceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.alias_path = Path(self.tmp.name) / 'aliases.json'
        self.device_path = Path(self.tmp.name) / 'device.json'
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), MockDevice)
        self.server.revision = 8
        self.server.rows = copy.deepcopy(ROWS)
        self.server.settings = {'version': 2, 'display_config': {'brightness_percent': 20},
                                'request_config': {'radius_requests': []}}
        self.server.error = self.server.put_error = 0
        self.server.etag = None
        self.server.bad_ack = False
        self.server.requests = []
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.connections = []

        def mock_connection(host, port, timeout):
            self.connections.append((host, port, timeout))
            self.assertEqual((host, port), (CONNECTION['ip'], 8787))
            return REAL_HTTP('127.0.0.1', self.server.server_port, timeout=timeout)

        self.patches = [patch.object(local_device.http.client, 'HTTPConnection', side_effect=mock_connection),
                        patch.object(control, 'ALIASES_PATH', self.alias_path),
                        patch.object(control, 'DEVICE_PATH', self.device_path),
                        patch.object(control, '_session', None),
                        patch.object(control, '_request', side_effect=AssertionError('cloud forbidden'))]
        for p in self.patches:
            p.start()
        self.client = local_device.Client(CONNECTION)

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.tmp.cleanup()

    def test_addresses_are_literal_lan_ipv4_only(self):
        for value in ['192.168.1.2', '10.5.4.3', '172.16.10.3', '169.254.2.3']:
            self.assertEqual(local_device.address(value), value)
        for value in ['127.0.0.1', '8.8.8.8', 'localhost', '192.168.1.2:80',
                      'http://192.168.1.2', '192.168.01.2', '::1', '0.0.0.0',
                      '2130706433', '100.64.0.1', '224.0.0.1', '192.168.0.0',
                      '192.168.255.255', None, 123]:
            with self.subTest(value=value), self.assertRaises(local_device.DeviceError):
                local_device.address(value)
        self.assertEqual(self.connections, [])

    def test_uppercase_key_is_normalized_before_authentication(self):
        result = control.connect_device(CONNECTION['ip'], TOKEN.upper())
        self.assertTrue(result['connected'])
        self.assertEqual(local_device.load_connection(self.device_path), CONNECTION)

    def test_connect_stores_key_but_response_never_returns_it(self):
        result = control.connect_device(**{'ip': CONNECTION['ip'], 'token': TOKEN})
        self.assertTrue(result['connected'])
        self.assertEqual(result['revision'], 8)
        self.assertNotIn(TOKEN, json.dumps(result))
        self.assertEqual(local_device.load_connection(self.device_path), CONNECTION)

    def test_failed_connect_preserves_previous_connection(self):
        local_device.save_connection(self.device_path, CONNECTION)
        previous = self.device_path.read_bytes()
        self.server.error = 401
        with self.assertRaisesRegex(local_device.DeviceError, 'key was rejected'):
            control.connect_device(CONNECTION['ip'], 'b2' * 32)
        self.assertEqual(self.device_path.read_bytes(), previous)

    def test_status_reports_actual_storage_revision_and_count(self):
        self.assertEqual(control.device_status()['configured'], False)
        local_device.save_connection(self.device_path, CONNECTION)
        result = control.device_status()
        self.assertEqual((result['revision'], result['alias_count']), (8, 1))
        self.assertTrue(result['device_confirmed'])
        self.assertNotIn(TOKEN, json.dumps(result))

    def test_alias_read_never_replaces_pc_rules(self):
        saved = control.save_aliases([], 0)
        self.assertEqual(self.client.aliases(control.clean_alias_list)['aliases'], ROWS)
        self.assertEqual(control.load_alias_document(), saved)

    def test_apply_increments_device_revision_independently_and_confirms_exact_rules(self):
        local_device.save_connection(self.device_path, CONNECTION)
        saved = control.save_aliases(ROWS, 0)
        result = control.apply_device_aliases(1, 8)
        self.assertEqual((result['revision'], result['aliases']), (9, ROWS))
        self.assertTrue(result['device_confirmed'])
        self.assertEqual(control.load_alias_document(), saved)
        self.assertEqual([r[0] for r in self.server.requests], ['GET', 'PUT'])
        self.assertEqual(self.server.requests[-1][2]['If-Match'], '"8"')

    def test_stale_browser_revision_never_sends_put(self):
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.apply([], 7, control.clean_alias_list)
        self.assertEqual(caught.exception.status, 409)
        self.assertEqual([r[0] for r in self.server.requests], ['GET'])
        self.assertEqual(self.server.rows, ROWS)

    def test_conflict_between_read_and_write_is_visible(self):
        self.server.put_error = 409
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.apply([], 8, control.clean_alias_list)
        self.assertEqual(caught.exception.status, 409)
        self.assertEqual(self.server.rows, ROWS)

    def test_nvs_failure_does_not_claim_ack_or_lose_saved_rules(self):
        local_device.save_connection(self.device_path, CONNECTION)
        saved = control.save_aliases([], 0)
        self.server.put_error = 503
        with self.assertRaisesRegex(local_device.DeviceError, 'No save confirmed'):
            control.apply_device_aliases(1, 8)
        self.assertEqual(control.load_alias_document(), saved)
        self.assertEqual(self.server.rows, ROWS)

    def test_wrong_ack_does_not_claim_device_confirmed(self):
        self.server.bad_ack = True
        with self.assertRaisesRegex(local_device.DeviceError, 'exact saved rules'):
            self.client.apply(ROWS, 8, control.clean_alias_list)

    def test_redirect_never_follows_and_response_details_are_sanitized(self):
        for code in [302, 401, 422, 500]:
            self.server.error = code
            with self.subTest(code=code), self.assertRaises(local_device.DeviceError) as caught:
                self.client.status()
            self.assertNotIn(TOKEN, str(caught.exception))
        self.assertEqual(len(self.connections), 4)

    def test_etag_required_and_numeric_revision_validation(self):
        self.server.etag = '"7"'
        with self.assertRaisesRegex(local_device.DeviceError, 'revision tag'):
            self.client.aliases(control.clean_alias_list)
        for rev in [True, -1, 0x100000000, '8', None]:
            with self.subTest(rev=rev), self.assertRaises(local_device.DeviceError):
                self.client.apply(ROWS, rev, control.clean_alias_list)

    def test_invalid_saved_revision_prevents_network_write(self):
        local_device.save_connection(self.device_path, CONNECTION)
        control.save_aliases(ROWS, 0)
        with self.assertRaises(local_device.DeviceError):
            control.apply_device_aliases(0, 8)
        self.assertEqual(self.connections, [])

    def test_offline_error_does_not_leak_address_key_or_exception(self):
        with patch.object(local_device.http.client, 'HTTPConnection', side_effect=OSError(TOKEN)):
            # Constructor failures are sanitized too.
            with self.assertRaises(local_device.DeviceError) as caught:
                self.client.status()
        self.assertNotIn(TOKEN, str(caught.exception))

    def test_response_size_and_timeout_are_bounded(self):
        class Response:
            status = 200

            def __init__(self, length=None):
                self.length = length
                self.stream = io.BytesIO(b'x' * (local_device.MAX_BODY + 1))

            def getheader(self, key):
                return self.length if key == 'Content-Length' else '"8"'

            def read1(self, length):
                return self.stream.read(length)

        for length in [None, str(local_device.MAX_BODY + 1), '-1', 'invalid']:
            conn = unittest.mock.Mock()
            conn.getresponse.return_value = Response(length)
            with self.subTest(length=length), patch.object(local_device.http.client, 'HTTPConnection', return_value=conn):
                with self.assertRaisesRegex(local_device.DeviceError, 'too large|malformed'):
                    self.client.status()
                self.assertTrue(conn.close.called)
        conn = unittest.mock.Mock()
        conn.request.side_effect = TimeoutError(TOKEN)
        with patch.object(local_device.http.client, 'HTTPConnection', return_value=conn):
            with self.assertRaises(local_device.DeviceError) as caught:
                self.client.status()
        self.assertNotIn(TOKEN, str(caught.exception))
        self.assertTrue(conn.close.called)

    def test_corrupt_credentials_do_not_leak_or_get_silently_overwritten(self):
        self.device_path.write_text(TOKEN)
        with self.assertRaises(local_device.DeviceError) as caught:
            control.device_status()
        self.assertNotIn(TOKEN, str(caught.exception))
        self.assertEqual(self.device_path.read_text(), TOKEN)

    def test_desk_endpoints_do_not_expose_keys_and_local_control_needs_no_cloud_session(self):
        desk = ThreadingHTTPServer(('127.0.0.1', 0), control.Handler)
        worker = threading.Thread(target=desk.serve_forever, daemon=True)
        worker.start()

        def request(method, path, data=None):
            conn = REAL_HTTP('127.0.0.1', desk.server_port, timeout=3)
            try:
                conn.request(method, path, json.dumps(data) if data is not None else None,
                             {'Content-Type': 'application/json'})
                response = conn.getresponse()
                return response.status, json.loads(response.read())
            finally:
                conn.close()

        try:
            code, body = request('POST', '/api/device/connect', CONNECTION)
            self.assertEqual(code, 200)
            self.assertNotIn(TOKEN, json.dumps(body))
            code, body = request('GET', '/api/session')
            self.assertEqual((code, body['ready'], body['source']), (200, True, 'local-device'))
            code, body = request('GET', '/api/config')
            self.assertEqual((code, body['config']['display_config']['brightness_percent']), (200, 20))
            baseline = copy.deepcopy(body['config'])
            updated = copy.deepcopy(baseline)
            updated['display_config']['brightness_percent'] = 45
            code, body = request('POST', '/api/config', {'config': updated, 'baseline': baseline})
            self.assertEqual((code, body['config']['display_config']['brightness_percent']), (200, 45))
            self.assertTrue(body['meta']['device_confirmed'])
            code, body = request('GET', '/api/device/settings')
            self.assertEqual((code, body['config']['display_config']['brightness_percent']), (200, 45))
            code, body = request('GET', '/api/device/aliases')
            self.assertEqual((code, body['aliases']), (200, ROWS))
            control.save_aliases([], 0)
            code, body = request('POST', '/api/device/aliases',
                                 {'revision': 1, 'expected_device_revision': 8})
            self.assertEqual((code, body['revision'], body['aliases']), (200, 9, []))
            self.assertTrue(body['device_confirmed'])
            code, body = request('POST', '/api/device/aliases',
                                 {'revision': 1, 'expected_device_revision': 8})
            self.assertEqual((code, body['device_confirmed']), (409, False))
            self.server.error = 401
            code, body = request('GET', '/api/device/status')
            self.assertEqual(code, 200)
            self.assertFalse(body['connected'])
            self.assertFalse(body['device_confirmed'])
            self.assertNotIn(TOKEN, json.dumps(body))
        finally:
            desk.shutdown()
            desk.server_close()
            worker.join()

    def test_pairing_export_requires_explicit_same_origin_post(self):
        local_device.save_connection(self.device_path, CONNECTION)
        desk=ThreadingHTTPServer(('127.0.0.1',0),control.Handler)
        worker=threading.Thread(target=desk.serve_forever,daemon=True);worker.start()
        def request(method,extra=None):
            conn=REAL_HTTP('127.0.0.1',desk.server_port,timeout=3)
            try:
                headers={'Content-Type':'application/json',**(extra or {})}
                conn.request(method,'/api/device/pairing','{}' if method=='POST' else None,headers)
                response=conn.getresponse();return response.status,json.loads(response.read())
            finally:conn.close()
        try:
            for method,headers in [('GET',{}),('POST',{}),('POST',{'X-Flightwall-Action':'export-pairing','Origin':'http://evil.example'})]:
                code,body=request(method,headers);self.assertNotEqual(code,200);self.assertNotIn(TOKEN,json.dumps(body))
            code,body=request('POST',{'X-Flightwall-Action':'export-pairing','Origin':'http://127.0.0.1:'+str(desk.server_port)})
            self.assertEqual(code,200);self.assertEqual(body,{'schema_version':1,**CONNECTION})
        finally:desk.shutdown();desk.server_close();worker.join()


if __name__ == '__main__':
    unittest.main()
