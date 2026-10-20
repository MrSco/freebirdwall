"""Packer tests on synthetic ESP32-S3 images. No device, no real key.

    py runtime/firmware/tests/test_package_owned.py -v
"""
import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import package_owned as po  # noqa: E402

KEY = bytes(range(1, 33))
TOKEN = KEY.hex()
PLACEHOLDER = po.MAGIC + struct.pack('<I', 0) + bytes(32)


def make_image(segments, chip=9, hash_appended=1):
    """segments: list of (address, data). Builds header, footer and SHA."""
    header = bytearray(24)
    header[0], header[1], header[2], header[3] = 0xE9, len(segments), 2, 0x3F
    struct.pack_into('<H', header, 12, chip)
    header[23] = hash_appended
    body = bytearray(header)
    value = 0xEF
    for address, data in segments:
        assert len(data) % 4 == 0
        body += struct.pack('<II', address, len(data)) + data
        for b in data:
            value ^= b
    body += bytes((15 - len(body)) % 16) + bytes([value])
    if hash_appended:
        body += hashlib.sha256(bytes(body)).digest()
    return bytes(body)


def drom(extra=b'', placeholder=True, size=512):
    data = bytearray(struct.pack('<I', po.APP_DESC_MAGIC) + bytes(size - 4))
    if placeholder:
        data[64:64 + 44] = PLACEHOLDER
    data[200:200 + len(extra)] = extra
    return bytes(data)


def good_image():
    return make_image([(0x3C080020, drom()), (0x3FC94730, bytes(64)), (0x42000020, bytes(256))])


class PackTests(unittest.TestCase):
    def test_packs_key_and_keeps_image_valid(self):
        image = good_image()
        candidate, at = po.pack(image, KEY)
        po.verify_integrity(candidate)
        self.assertEqual(candidate[at:at + 8], po.MAGIC)
        self.assertEqual(struct.unpack_from('<I', candidate, at + 8)[0], 1)
        self.assertEqual(candidate[at + 12:at + 44], KEY)
        self.assertEqual(len(candidate), len(image))
        changed = [i for i, (a, b) in enumerate(zip(image, candidate)) if a != b]
        footer = len(image) - 33
        self.assertTrue(all(at + 8 <= i < at + 44 or i >= footer for i in changed),
                        'only the descriptor, checksum byte and SHA may change')

    def test_deterministic(self):
        self.assertEqual(po.pack(good_image(), KEY), po.pack(good_image(), KEY))

    def test_desk_validator_accepts_packaged_and_rejects_unpackaged(self):
        candidate, _ = po.pack(good_image(), KEY)
        self.assertEqual(po.desk_validate(candidate, TOKEN), hashlib.sha256(candidate).hexdigest())
        with self.assertRaises(po.PackageError):
            po.desk_validate(good_image(), TOKEN)

    def test_device_validator_across_chunkings(self):
        candidate, _ = po.pack(good_image(), KEY)
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = po.build_device_checker(work)
            for chunk in (1, 3, 13, 44, 45, 4096, len(candidate)):
                row = po.device_check(exe, candidate, TOKEN, chunk, work)
                self.assertTrue(row['accepted'], (chunk, row))
            self.assertFalse(po.device_check(exe, candidate, bytes(32 * [7]).hex(), 64, work)['accepted'])
            self.assertFalse(po.device_check(exe, good_image(), TOKEN, 64, work)['accepted'])

    def refuse(self, image, key=KEY, message=None):
        with self.assertRaises(po.PackageError) as ctx:
            po.pack(image, key)
        if message:
            self.assertIn(message, str(ctx.exception))

    def test_refuses_missing_placeholder(self):
        self.refuse(make_image([(0x3C080020, drom(placeholder=False))]), message='no owner descriptor')

    def test_refuses_duplicate_magic(self):
        self.refuse(make_image([(0x3C080020, drom()), (0x42000020, drom(extra=po.MAGIC))]),
                    message='unique')

    def test_bare_magic_elsewhere_is_not_a_descriptor(self):
        # e.g. the update validator's own search constant, followed by other data
        image = make_image([(0x3C080020, drom()), (0x42000020, drom(extra=po.MAGIC + struct.pack('<I', 0x428A2F98), placeholder=False))])
        candidate, _ = po.pack(image, KEY)
        self.assertEqual(candidate.count(po.MAGIC + struct.pack('<I', 1)), 1)

    def test_refuses_already_packaged(self):
        candidate, _ = po.pack(good_image(), KEY)
        self.refuse(candidate, message='already packaged')

    def test_refuses_non_empty_placeholder(self):
        data = bytearray(drom())
        data[64 + 12] = 1
        self.refuse(make_image([(0x3C080020, bytes(data))]), message='placeholder')

    def test_refuses_placeholder_across_segments(self):
        first = bytearray(struct.pack('<I', po.APP_DESC_MAGIC) + bytes(508))
        first[-20:] = PLACEHOLDER[:20]
        second = PLACEHOLDER[20:] + bytes(4)
        image = make_image([(0x3C080020, bytes(first)), (0x42000020, second)])
        # The 8-byte segment header sits between the halves, so the magic is
        # whole but the slot is not contiguous image data.
        self.refuse(image, message='straddles')

    def test_refuses_corrupt_inputs(self):
        image = bytearray(good_image())
        image[-33] ^= 1
        self.refuse(bytes(image), message='checksum')
        image = bytearray(good_image())
        image[-1] ^= 1
        self.refuse(bytes(image), message='SHA')
        self.refuse(make_image([(0x3C080020, drom())], chip=2), message='ESP32-S3')
        self.refuse(make_image([(0x3C080020, drom())], hash_appended=0), message='SHA')
        no_desc = make_image([(0x3C080020, bytes(64) + PLACEHOLDER + bytes(400))])
        self.refuse(no_desc, message='app descriptor')

    def test_refuses_bad_keys(self):
        self.refuse(good_image(), key=bytes(32), message='non-zero')
        self.refuse(good_image(), key=KEY[:31], message='32')


class CliTests(unittest.TestCase):
    def test_cli_writes_private_release_without_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'app.bin').write_bytes(good_image())
            (tmp / 'device.json').write_text(json.dumps({'ip': '10.0.0.2', 'token': TOKEN}))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = po.main(['--image', str(tmp / 'app.bin'), '--connection', str(tmp / 'device.json'),
                                '--out', str(tmp / 'pkg')])
            self.assertEqual(code, 0)
            folder = next((tmp / 'pkg').iterdir())
            release = json.loads((folder / 'release.json').read_text())
            candidate = (folder / 'candidate.bin').read_bytes()
            self.assertEqual(release['candidate_sha256'], hashlib.sha256(candidate).hexdigest())
            self.assertEqual(release['status'], 'untested-on-hardware')
            self.assertTrue(release['checks']['device_validator_rejects_wrong_key'])
            text = (folder / 'release.json').read_text() + out.getvalue()
            self.assertNotIn(TOKEN, text)
            self.assertNotIn(TOKEN.upper(), text)

    def test_cli_bad_connection_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'app.bin').write_bytes(good_image())
            (tmp / 'device.json').write_text('{"ip": "10.0.0.2"}')
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = po.main(['--image', str(tmp / 'app.bin'), '--connection', str(tmp / 'device.json'),
                                '--out', str(tmp / 'pkg')])
            self.assertEqual(code, 1)
            self.assertFalse((tmp / 'pkg').exists())


if __name__ == '__main__':
    unittest.main()
