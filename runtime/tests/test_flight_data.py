"""Portable source-runtime tests; no device, credentials or network required."""
import hashlib,json,shutil,subprocess,tempfile,unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

class FlightDataTests(unittest.TestCase):
    def test_pinned_dependency(self):
        folder=ROOT/'third_party/ArduinoJson'
        pin=json.loads((folder/'SOURCE.json').read_text())
        self.assertEqual(hashlib.sha256((folder/'ArduinoJson-v6.21.5.h').read_bytes()).hexdigest(),pin['header_sha256'])

    def test_portable_adapters(self):
        self.compile_run('flight_data_test.cpp')

    def test_provider_policy(self):
        self.compile_run('provider_policy_test.cpp')

    def test_stock_wire(self):
        self.compile_run('provider_wire_test.cpp')

    def test_streaming_adsb(self):
        self.compile_run('stream_adsb_test.cpp')

    def test_http_body(self):
        self.compile_run('http_body_test.cpp')

    def test_community_cache(self):
        self.compile_run('community_cache_test.cpp')

    def compile_run(self,test):
        cc=shutil.which('g++')
        if not cc:self.skipTest('g++ unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'flight_data_test.exe'
            subprocess.run([cc,'-std=c++17','-Os','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',
                            str(ROOT/'src/flight_data.cpp'),str(ROOT/'src/provider_policy.cpp'),
                            str(ROOT/'src/community_config.cpp'),str(ROOT/'src/community_cache.cpp'),
                            str(ROOT/'esp32/provider_wire.cpp'),str(ROOT/'esp32/stream_adsb.cpp'),
                            str(ROOT/'esp32/http_body.cpp'),
                            str(ROOT/'tests'/test),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
