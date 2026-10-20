"""Package the owned firmware for this device's Wi-Fi updater. Offline only.

    py tools/package_owned.py [--image build/freebirdwall.bin]
                              [--connection ../../desk/device.json]
                              [--out build/package]

The build leaves a 44-byte placeholder descriptor (owner_descriptor.c). The
device's updater accepts only an image containing exactly one descriptor
"FWOTA1\\0\\0" + version 1 + this device's 32-byte key. This script:

  1. strictly parses the built ESP32-S3 image and checks its checksum and SHA;
  2. finds the single placeholder, writes version 1 and the key into it;
  3. recomputes the image checksum byte and appended SHA-256;
  4. runs the desk's own validator (desk/firmware_device.py);
  5. runs the device's own update validator (shared/owner_ota.c,
     compiled for the desktop) over the upload stream at several chunk sizes,
     and confirms it rejects a wrong key and the unpackaged image;
  6. writes candidate.bin + release.json into a private, git-ignored folder.

The packaged image contains the device key: keep it private. The key is never
printed; release.json records only a short SHA-256 fingerprint of it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
REPO = FIRMWARE.parent.parent
TOOL_SRC = REPO / 'shared'
CONTROL = REPO / 'desk'

MAGIC = b'FWOTA1\0\0'
DESCRIPTOR_BYTES = 44
SLOT_BYTES = 0x2C0000
APP_DESC_MAGIC = 0xABCD5432
PROFILE = 'owned-skeleton-v1'
CHUNKS = (1, 7, 512, 4096, 65536)


class PackageError(Exception):
    pass


def parse_image(image: bytes):
    """Strict ESP32-S3 app parse. Returns (segments, footer_offset).

    segments: list of (address, data_offset, size)."""
    if len(image) < 24 + 8 + 33 or len(image) > SLOT_BYTES:
        raise PackageError('image size %d is outside the app slot' % len(image))
    if image[0] != 0xE9 or not 1 <= image[1] <= 16:
        raise PackageError('not an ESP application image')
    if struct.unpack_from('<H', image, 12)[0] != 9:
        raise PackageError('image is not built for the ESP32-S3 (chip id 9)')
    if image[23] != 1:
        raise PackageError('image has no appended SHA-256')
    pos, segments = 24, []
    for _ in range(image[1]):
        if pos + 8 > len(image):
            raise PackageError('segment header truncated')
        address, size = struct.unpack_from('<II', image, pos)
        pos += 8
        if not size or size % 4 or pos + size > len(image):
            raise PackageError('segment bounds invalid')
        segments.append((address, pos, size))
        pos += size
    footer = (pos // 16) * 16 + 15
    if len(image) != footer + 33 or any(image[pos:footer]):
        raise PackageError('image length or padding invalid')
    return segments, footer


def checksum(image: bytes, segments) -> int:
    value = 0xEF
    for _addr, offset, size in segments:
        for byte in image[offset:offset + size]:
            value ^= byte
    return value


def verify_integrity(image: bytes):
    segments, footer = parse_image(image)
    if image[footer] != checksum(image, segments):
        raise PackageError('image checksum byte is wrong')
    if hashlib.sha256(image[:footer + 1]).digest() != image[footer + 1:]:
        raise PackageError('image appended SHA-256 is wrong')
    first = segments[0]
    if struct.unpack_from('<I', image, first[1])[0] != APP_DESC_MAGIC or first[2] < 256:
        raise PackageError('first segment does not start with the ESP app descriptor')
    return segments, footer


def find_placeholder(image: bytes, segments) -> int:
    """The single descriptor-shaped occurrence: the magic followed by version 0
    or 1. The bare magic also appears as data elsewhere (the update validator,
    owner_ota.c, keeps it as the constant it searches for); the desk and device
    validators count only magic + version 1, so those copies never match."""
    hits = []
    start = 0
    while True:
        at = image.find(MAGIC, start)
        if at < 0:
            break
        if at + 12 <= len(image) and struct.unpack_from('<I', image, at + 8)[0] in (0, 1):
            hits.append(at)
        start = at + 1
    if not hits:
        raise PackageError('no owner descriptor slot in the image; was owner_descriptor.c built in?')
    if len(hits) > 1:
        raise PackageError('descriptor occurs %d times; it must be unique' % len(hits))
    at = hits[0]
    if not any(offset <= at and at + DESCRIPTOR_BYTES <= offset + size for _a, offset, size in segments):
        raise PackageError('descriptor slot straddles a segment boundary')
    version = struct.unpack_from('<I', image, at + 8)[0]
    key = image[at + 12:at + DESCRIPTOR_BYTES]
    if version == 1:
        raise PackageError('image is already packaged (descriptor version 1); package the unpackaged build output')
    if version != 0 or any(key):
        raise PackageError('descriptor slot is not the expected empty placeholder')
    return at


def pack(image: bytes, key: bytes):
    """Returns (candidate, descriptor_offset). Pure function; no IO."""
    if len(key) != 32 or not any(key):
        raise PackageError('device key must be 32 non-zero bytes')
    segments, footer = verify_integrity(image)
    at = find_placeholder(image, segments)
    out = bytearray(image)
    out[at + 8:at + 12] = struct.pack('<I', 1)
    out[at + 12:at + DESCRIPTOR_BYTES] = key
    out[footer] = checksum(out, segments)
    out[footer + 1:] = hashlib.sha256(bytes(out[:footer + 1])).digest()
    candidate = bytes(out)
    verify_integrity(candidate)
    return candidate, at


def app_description(image: bytes) -> dict:
    base = 24 + 8
    def text(offset, size):
        return image[base + offset:base + offset + size].split(b'\0', 1)[0].decode('ascii', 'replace')
    return {'version': text(16, 32), 'project_name': text(48, 32), 'compile_time': text(80, 16),
            'compile_date': text(96, 16), 'idf_ver': text(112, 32),
            'app_elf_sha256': image[base + 144:base + 176].hex()}


def load_key(connection: Path) -> tuple[str, bytes]:
    try:
        token = json.loads(connection.read_text(encoding='utf-8'))['token']
        key = bytes.fromhex(token)
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as exc:
        raise PackageError('could not read the device key from %s (%s)' % (connection, type(exc).__name__))
    if len(key) != 32 or not any(key):
        raise PackageError('device key in %s is not 32 non-zero bytes' % connection)
    return token, key


def desk_validate(candidate: bytes, token: str) -> str:
    sys.path.insert(0, str(CONTROL))
    try:
        import firmware_device  # the desk's exact upload validator
    finally:
        sys.path.pop(0)
    try:
        return firmware_device.validate_image(candidate, token)
    except Exception as exc:  # DeviceError carries a user-facing message
        raise PackageError('desk validator rejected the image: %s' % exc)


def build_device_checker(workdir: Path) -> Path:
    gcc = shutil.which('gcc')
    if not gcc:
        raise PackageError('a desktop gcc is needed to run the device validator check')
    exe = workdir / ('owner_ota_check' + ('.exe' if os.name == 'nt' else ''))
    cmd = [gcc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(TOOL_SRC),
           str(HERE / 'owner_ota_check.c'), str(TOOL_SRC / 'owner_ota.c'),
           str(TOOL_SRC / 'owner_sha256.c'), '-o', str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        raise PackageError('could not build the device validator check:\n' + result.stderr)
    return exe


def device_check(exe: Path, image: bytes, key_hex: str, chunk: int, workdir: Path) -> dict:
    path = workdir / 'probe.bin'
    path.write_bytes(image)
    result = subprocess.run([str(exe), str(path), key_hex, str(chunk)], capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise PackageError('device validator check failed to run: ' + result.stderr.strip())
    return json.loads(result.stdout)


def git_identity() -> dict:
    def git(*args):
        r = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    status = git('status', '--porcelain', '--', 'runtime')
    return {'commit': git('rev-parse', 'HEAD'), 'branch': git('rev-parse', '--abbrev-ref', 'HEAD'),
            'runtime_tree_dirty': bool(status) if status is not None else None}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=FIRMWARE / 'build' / 'freebirdwall.bin')
    ap.add_argument('--connection', type=Path, default=CONTROL / 'device.json')
    ap.add_argument('--out', type=Path, default=FIRMWARE / 'build' / 'package')
    a = ap.parse_args(argv)

    try:
        image = a.image.read_bytes()
    except OSError:
        print('ERROR: built image not found at %s (run idf.py build)' % a.image, file=sys.stderr)
        return 2
    try:
        token, key = load_key(a.connection)
        candidate, at = pack(image, key)
        digest = desk_validate(candidate, token)

        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = build_device_checker(work)
            accepted = []
            for chunk in CHUNKS + (len(candidate),):
                row = device_check(exe, candidate, key.hex(), chunk, work)
                if not row['accepted'] or row['written'] != len(candidate) or row['selected'] != 1:
                    raise PackageError('device validator rejected the image at chunk %d: %s' % (chunk, row['reason']))
                accepted.append(chunk)
            wrong = bytes(b ^ 0xFF for b in key)
            row = device_check(exe, candidate, wrong.hex(), 4096, work)
            if row['accepted'] or row['reason'] != 'descriptor-missing-duplicate-or-wrong-key':
                raise PackageError('device validator did not reject a wrong key')
            row = device_check(exe, image, key.hex(), 4096, work)
            if row['accepted']:
                raise PackageError('device validator accepted the unpackaged image')
    except PackageError as exc:
        print('ERROR: %s' % exc, file=sys.stderr)
        return 1

    folder = a.out / ('%s-%s' % (PROFILE, digest[:12]))
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'candidate.bin').write_bytes(candidate)
    release = {
        'schema_version': 1,
        'profile': PROFILE,
        'status': 'untested-on-hardware',
        'candidate_sha256': digest,
        'candidate_bytes': len(candidate),
        'unpackaged_sha256': hashlib.sha256(image).hexdigest(),
        'descriptor_offset': at,
        'key_fingerprint': hashlib.sha256(key).hexdigest()[:16],
        'app': app_description(candidate),
        'source': git_identity(),
        'checks': {
            'image_integrity': True,
            'desk_validator': True,
            'device_validator_chunks_accepted': accepted,
            'device_validator_rejects_wrong_key': True,
            'device_validator_rejects_unpackaged_image': True,
            'not_checked_offline': 'the board-side ESP-IDF image verification inside end(); '
                                   'the desk validator independently checks the same checksum and SHA footer',
        },
        'behaviour': [
            'Measurement probe only: no panel output, settings, aliases or update endpoint.',
            'Never confirms itself; restarts after its probe (or 120 s) so the bootloader rolls back.',
            'The uploader\'s confirmation step will fail (no endpoint); that is expected and keeps rollback.',
            'Never writes NVS (driver storage RAM-only, PHY calibration storage disabled).',
        ],
    }
    (folder / 'release.json').write_text(json.dumps(release, indent=2) + '\n', encoding='utf-8')

    print('Packaged %s' % PROFILE)
    print('  candidate   %s' % (folder / 'candidate.bin'))
    print('  sha256      %s  (%d bytes)' % (digest, len(candidate)))
    print('  descriptor  offset 0x%x, key fingerprint %s' % (at, release['key_fingerprint']))
    print('  desk validator: accepted')
    print('  device validator: accepted at chunks %s; wrong key and unpackaged image rejected' % accepted)
    print('Keep candidate.bin private: it contains the device key.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
