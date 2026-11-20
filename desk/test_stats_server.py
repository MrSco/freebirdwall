import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import stats_server


class StatsServerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.srv = ThreadingHTTPServer(('127.0.0.1', 0), stats_server.make_handler(Path(self.tmp.name)))
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def url(self, path):
        return f'http://127.0.0.1:{self.port}{path}'

    def test_round_trip_and_validation(self):
        with urllib.request.urlopen(self.url('/health')) as r:
            h = json.loads(r.read())
            self.assertTrue(h['ok'])
            self.assertFalse(h['has_checkpoint'])

        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(self.url('/checkpoint'))
        self.assertEqual(ctx.exception.code, 404)

        # Reject invalid magic
        req = urllib.request.Request(self.url('/checkpoint'), data=b'BAD!' + b'\x00' * 32, method='PUT')
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(req)
        self.assertEqual(ctx.exception.code, 400)

        # Save valid checkpoint
        payload = b'FWAN' + bytes(range(64))
        req = urllib.request.Request(self.url('/checkpoint'), data=payload, method='PUT')
        with urllib.request.urlopen(req) as r:
            res = json.loads(r.read())
            self.assertTrue(res['ok'])
            self.assertEqual(res['bytes'], len(payload))

        # Restore exact bytes
        with urllib.request.urlopen(self.url('/checkpoint')) as r:
            self.assertEqual(r.read(), payload)

        with urllib.request.urlopen(self.url('/health')) as r:
            h = json.loads(r.read())
            self.assertTrue(h['has_checkpoint'])
            self.assertEqual(h['bytes'], len(payload))
            self.assertEqual(h['backups'], 1)


if __name__ == '__main__':
    unittest.main()
