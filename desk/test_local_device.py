"""LAN client boundaries against an isolated mock device. No saved keys."""
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

import local_device

TOKEN = 'a1' * 32
CONNECTION = {'ip': '192.168.50.20', 'token': TOKEN}
ROWS = [{'kind': 'iata', 'from': 'DJT', 'to': 'PBI'}]
REAL_HTTP = http.client.HTTPConnection


def clean(rows):
    """The caller's alias check (Client.aliases/apply take one): a list of
    {kind, from, to} strings, else ValueError."""
    if not isinstance(rows, list) or not all(
            isinstance(r, dict) and set(r) == {'kind', 'from', 'to'} and all(isinstance(v, str) for v in r.values())
            for r in rows):
        raise ValueError('rows')
    return copy.deepcopy(rows)


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

        self.patch = patch.object(local_device.http.client, 'HTTPConnection', side_effect=mock_connection)
        self.patch.start()
        self.client = local_device.Client(CONNECTION)

    def tearDown(self):
        self.patch.stop()
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
        connection = local_device.credentials(CONNECTION['ip'], TOKEN.upper())
        self.assertEqual(connection, CONNECTION)
        self.assertTrue(local_device.Client(connection).status()['connected'])
        for bad in ['a1' * 31, 'g1' * 32, None]:
            with self.subTest(token=bad), self.assertRaises(local_device.DeviceError):
                local_device.credentials(CONNECTION['ip'], bad)

    def test_connection_file_round_trip_is_private_and_exact(self):
        self.assertIsNone(local_device.load_connection(self.device_path))
        local_device.save_connection(self.device_path, {'ip': CONNECTION['ip'], 'token': TOKEN.upper()})
        self.assertEqual(local_device.load_connection(self.device_path), CONNECTION)
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [self.device_path])   # no temporary left behind

    def test_rejected_key_is_explained_without_the_key(self):
        self.server.error = 401
        with self.assertRaisesRegex(local_device.DeviceError, 'key was rejected') as caught:
            local_device.Client({'ip': CONNECTION['ip'], 'token': 'b2' * 32}).status()
        self.assertNotIn('b2' * 32, str(caught.exception))

    def test_status_reports_actual_storage_revision_and_count(self):
        result = self.client.status()
        self.assertEqual((result['revision'], result['alias_count'], result['storage_ready']), (8, 1, True))
        self.assertTrue(result['device_confirmed'])
        self.assertNotIn(TOKEN, json.dumps(result))
        with self.assertRaisesRegex(local_device.DeviceError, 'No local device'):
            local_device.Client(None)

    def test_alias_read_returns_the_device_rules(self):
        result = self.client.aliases(clean)
        self.assertEqual((result['revision'], result['aliases']), (8, ROWS))
        self.server.rows = [{'kind': 'iata'}]
        with self.assertRaisesRegex(local_device.DeviceError, 'invalid rename rules'):
            self.client.aliases(clean)

    def test_apply_increments_device_revision_and_confirms_exact_rules(self):
        result = self.client.apply([], 8, clean)
        self.assertEqual((result['revision'], result['aliases'], result['alias_count']), (9, [], 0))
        self.assertTrue(result['device_confirmed'])
        self.assertEqual(self.server.rows, [])
        self.assertEqual([r[0] for r in self.server.requests], ['GET', 'PUT'])
        self.assertEqual(self.server.requests[-1][2]['If-Match'], '"8"')

    def test_stale_revision_never_sends_put(self):
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.apply([], 7, clean)
        self.assertEqual(caught.exception.status, 409)
        self.assertEqual([r[0] for r in self.server.requests], ['GET'])
        self.assertEqual(self.server.rows, ROWS)

    def test_conflict_between_read_and_write_is_visible(self):
        self.server.put_error = 409
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.apply([], 8, clean)
        self.assertEqual(caught.exception.status, 409)
        self.assertEqual(self.server.rows, ROWS)

    def test_nvs_failure_does_not_claim_a_save(self):
        self.server.put_error = 503
        with self.assertRaisesRegex(local_device.DeviceError, 'No save confirmed') as caught:
            self.client.apply([], 8, clean)
        self.assertEqual(caught.exception.status, 503)
        self.assertEqual(self.server.rows, ROWS)

    def test_wrong_ack_does_not_claim_device_confirmed(self):
        self.server.bad_ack = True
        with self.assertRaisesRegex(local_device.DeviceError, 'exact saved rules'):
            self.client.apply(ROWS, 8, clean)

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
            self.client.aliases(clean)
        for rev in [True, -1, 0x100000000, '8', None]:
            with self.subTest(rev=rev), self.assertRaises(local_device.DeviceError):
                self.client.apply(ROWS, rev, clean)

    def test_exhausted_revision_never_writes(self):
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.apply(ROWS, local_device.MAX_REVISION, clean)
        self.assertEqual(caught.exception.status, 409)
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
            local_device.load_connection(self.device_path)
        self.assertNotIn(TOKEN, str(caught.exception))
        self.assertEqual(self.device_path.read_text(), TOKEN)

    def test_settings_read_and_save_with_the_device_origin(self):
        self.assertEqual(self.client.settings()['display_config']['brightness_percent'], 20)
        doc = copy.deepcopy(self.server.settings)
        doc['display_config']['brightness_percent'] = 45
        saved = self.client.save_settings(doc)
        self.assertEqual(saved['display_config']['brightness_percent'], 45)
        self.assertEqual(self.server.settings, doc)
        self.assertEqual(self.server.requests[-1][2]['Origin'], 'http://192.168.50.20:8787')

    def test_settings_shape_and_size_are_checked_before_sending(self):
        sent = len(self.server.requests)
        for bad in [None, {'display_config': {}}, {'display_config': [], 'request_config': {}}]:
            with self.subTest(doc=bad), self.assertRaises(local_device.DeviceError):
                self.client.save_settings(bad)
        with self.assertRaises(local_device.DeviceError) as caught:
            self.client.save_settings({'display_config': {'x': 'y' * 4096}, 'request_config': {}})
        self.assertEqual(caught.exception.status, 413)
        self.assertEqual(len(self.server.requests), sent)
        self.server.settings = {'display_config': {}}
        with self.assertRaisesRegex(local_device.DeviceError, 'invalid settings'):
            self.client.settings()


if __name__ == '__main__':
    unittest.main()
