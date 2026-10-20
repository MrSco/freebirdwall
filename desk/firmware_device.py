"""Owner firmware OTA client. Literal LAN addresses, one upload, verified restart."""
from __future__ import annotations

import hashlib
import hmac
import http.client
import json
import re
import struct
import time

import local_device

MAX_IMAGE = 0x2c0000
MAX_RESPONSE = 16384
LOG_MAX = 8192            # the firmware keeps 4 KiB
DESCRIPTOR = b'FWOTA1\0\0'
POLL_SECONDS = 60
DeviceError = local_device.DeviceError


def sha256(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise DeviceError('Read the current firmware before updating.', 400)
    return value


def validate_image(image, token):
    """Strict ESP32-S3 unsigned application with its SHA footer and owner key.

    ESP checksums cover segment data; the appended digest covers everything
    through the checksum. No partition dumps, trailing padding, or other board
    keys are accepted. The board repeats these checks before selecting a slot.
    """
    if not isinstance(image, bytes) or not 24 <= len(image) <= MAX_IMAGE:
        raise DeviceError('Choose an owner firmware application .bin within the slot size.', 400)
    if (image[0] != 0xe9 or not 1 <= image[1] <= 16
            or struct.unpack_from('<H', image, 12)[0] != 9 or image[23] != 1):
        raise DeviceError('Firmware must be an ESP32-S3 application with an appended SHA-256.', 400)
    pos, checksum = 24, 0xef
    descriptors = []
    for _ in range(image[1]):
        if pos + 8 > len(image):
            raise DeviceError('Firmware segment header is truncated.', 400)
        address, size = struct.unpack_from('<II', image, pos)
        pos += 8
        if not size or size % 4 or address + size > 0xffffffff or pos + size > len(image):
            raise DeviceError('Firmware segment bounds are invalid.', 400)
        segment = image[pos:pos + size]
        for value in segment:
            checksum ^= value
        start = 0
        while True:
            offset = segment.find(DESCRIPTOR + struct.pack('<I', 1), start)
            if offset < 0:
                break
            descriptors.append(segment[offset:offset + 44])
            start = offset + len(DESCRIPTOR)
        pos += size
    footer = (pos // 16) * 16 + 15
    if len(image) != footer + 33 or any(image[pos:footer]) or image[footer] != checksum:
        raise DeviceError('Firmware length, padding, or ESP checksum is invalid.', 400)
    if not hmac.compare_digest(hashlib.sha256(image[:footer + 1]).digest(), image[footer + 1:]):
        raise DeviceError('Firmware appended SHA-256 is invalid.', 400)
    expected = DESCRIPTOR + struct.pack('<I', 1) + bytes.fromhex(token)
    if len(descriptors) != 1 or not hmac.compare_digest(descriptors[0], expected):
        raise DeviceError('Choose OTA-capable owner firmware built for this device key.', 400)
    return hashlib.sha256(image).hexdigest()


class Client:
    def __init__(self, connection):
        self.connection = local_device.Client(connection).connection

    def _request(self, method, image=None, expected=None, digest=None, timeout=3, path='/v1/firmware'):
        headers = {'Authorization': 'Bearer ' + self.connection['token'],
                   'Accept': 'application/json', 'Connection': 'close'}
        if image is not None:
            headers.update({'Content-Type': 'application/octet-stream',
                            'If-Match': '"' + expected + '"', 'X-Firmware-SHA256': digest})
        elif expected is not None:
            headers.update({'If-Match': '"' + expected + '"', 'Content-Length': '0'})
        conn = None
        try:
            conn = http.client.HTTPConnection(self.connection['ip'], local_device.PORT, timeout=timeout)
            conn.request(method, path, body=image, headers=headers)
            response = conn.getresponse()
            if response.status != 200:
                if response.status in (401, 403):
                    raise DeviceError('Device key was rejected. Reconnect using the provisioned key.', 401)
                if response.status in (409, 412):
                    raise DeviceError('Firmware changed or an update is busy. Read firmware status again.', 409)
                if response.status in (404, 405, 501):
                    raise DeviceError('Installed firmware does not support Wi-Fi firmware updates.', 501)
                if 300 <= response.status < 400:
                    raise DeviceError('Device redirected the request. Redirects are disabled.')
                raise DeviceError('Firmware request was rejected (HTTP %d). No update confirmed.' % response.status)
            length = response.getheader('Content-Length')
            if length is not None and (not length.isdigit() or int(length) > MAX_RESPONSE):
                raise DeviceError('Device firmware response is too large or malformed.')
            deadline, raw = time.monotonic() + timeout, bytearray()
            while len(raw) <= MAX_RESPONSE:
                if time.monotonic() > deadline:
                    raise DeviceError('Device firmware response timed out.')
                chunk = response.read1(min(2048, MAX_RESPONSE + 1 - len(raw)))
                if not chunk:
                    break
                raw.extend(chunk)
            if len(raw) > MAX_RESPONSE:
                raise DeviceError('Device firmware response is too large.')
            body = json.loads(raw.decode('utf-8'))
            if (not isinstance(body, dict) or type(body.get('api_version')) is not int
                    or body['api_version'] != 1 or type(body.get('schema_version')) is not int
                    or body['schema_version'] != 1):
                raise DeviceError('Device returned an unsupported firmware response.')
            return body
        except DeviceError:
            raise
        except (OSError, http.client.HTTPException, UnicodeError, ValueError, TypeError):
            raise DeviceError('Device is offline or returned an invalid firmware response. No update confirmed.') from None
        finally:
            if conn is not None:
                conn.close()

    def status(self, timeout=3):
        body = self._request('GET', timeout=timeout)
        valid_hash = isinstance(body.get('running_sha256'), str) and re.fullmatch('[0-9a-f]{64}', body['running_sha256'])
        if (body.get('ota_supported') is not True or not valid_hash
                or body.get('running_slot') not in ('app0', 'app1')
                or body.get('boot_slot') not in ('app0', 'app1')
                or type(body.get('partition_size')) is not int
                or not 24 <= body['partition_size'] <= MAX_IMAGE
                or type(body.get('rollback_supported')) is not bool
                or type(body.get('update_busy')) is not bool
                or type(body.get('pending_confirmation')) is not bool):
            raise DeviceError('Device returned an invalid firmware status.')
        # Only return this allowlist. No device-supplied error or key can leak.
        return {key: body[key] for key in ('api_version', 'schema_version', 'ota_supported',
                'running_slot', 'running_sha256', 'partition_size', 'boot_slot',
                'rollback_supported', 'update_busy', 'pending_confirmation')}

    def update(self, image, expected):
        expected = sha256(expected)
        digest = validate_image(image, self.connection['token'])
        current = self.status()
        if current['running_sha256'] != expected:
            raise DeviceError('Firmware changed. Read firmware status before uploading again.', 409)
        if current['update_busy'] or current['pending_confirmation'] or current['running_slot'] != current['boot_slot']:
            raise DeviceError('A firmware update is already pending. Wait and read firmware status.', 409)
        if len(image) > current['partition_size']:
            raise DeviceError('Firmware is larger than the device application slot.', 400)
        target = 'app0' if current['running_slot'] == 'app1' else 'app1'
        # Exactly one PUT: a lost receipt may still mean the board selected the
        # new slot. Never retry a firmware write after an ambiguous disconnect.
        receipt = self._request('PUT', image, expected, digest, timeout=45)
        if (receipt.get('accepted') is not True or receipt.get('sha256') != digest
                or type(receipt.get('bytes')) is not int or receipt['bytes'] != len(image)
                or receipt.get('target_slot') != target or receipt.get('rebooting') is not True):
            raise DeviceError('Upload receipt is invalid. Read firmware status; do not retry automatically.')
        deadline = time.monotonic() + POLL_SECONDS
        while time.monotonic() < deadline:
            try:
                status = self.status(timeout=max(0.1, min(3, deadline - time.monotonic())))
                if (status['running_sha256'] == digest and status['running_slot'] == target
                        and status['boot_slot'] == target and not status['update_busy']):
                    if status['pending_confirmation']:
                        self._request('POST', expected=digest,
                                      path='/v1/firmware/confirm',timeout=3)
                        status=self.status()
                    if status['pending_confirmation']:
                        continue
                    if status['running_sha256'] != digest or status['running_slot'] != target:
                        continue
                    return {'accepted': True, 'device_confirmed': True, 'reboot_confirmed': True,
                            'sha256': digest, 'bytes': len(image), 'target_slot': target,
                            'firmware': status}
            except DeviceError:
                pass  # A reboot temporarily closes the socket. The write is not repeated.
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(1, remaining))
        return {'accepted': True, 'device_confirmed': False, 'reboot_confirmed': False,
                'sha256': digest, 'bytes': len(image), 'target_slot': target,
                'message': 'Upload accepted, but restart is unconfirmed. Read firmware status before another upload.'}

    def log(self, timeout=10):
        """The owned firmware's kept status lines (GET /v1/log): FWSTATE/FWMEM
        lines from RTC memory, oldest first, so the wall can be watched over
        Wi-Fi without a USB console. Plain text, at most LOG_MAX bytes."""
        conn = None
        try:
            conn = http.client.HTTPConnection(self.connection['ip'], local_device.PORT, timeout=timeout)
            conn.request('GET', '/v1/log', headers={'Authorization': 'Bearer ' + self.connection['token'],
                                                    'Accept': 'text/plain', 'Connection': 'close'})
            response = conn.getresponse()
            if response.status in (401, 403):
                raise DeviceError('Device key was rejected. Reconnect using the provisioned key.', 401)
            if response.status in (404, 405, 501):
                raise DeviceError('The installed firmware has no Wi-Fi log (owned firmware only).', 501)
            if response.status != 200:
                raise DeviceError('Log request was rejected (HTTP %d).' % response.status)
            raw = response.read(LOG_MAX + 1)
            if len(raw) > LOG_MAX:
                raise DeviceError('Device log is too large.')
            return raw.decode('utf-8', errors='replace')
        except DeviceError:
            raise
        except (OSError, http.client.HTTPException):
            raise DeviceError('Device is offline or returned an invalid log.') from None
        finally:
            if conn is not None:
                conn.close()

    def switch_slot(self, expected):
        """Boot the image in the other slot (owned firmware's /v1/firmware/switch).

        Used to go from the owned firmware back to stock without a download:
        the stock image stays in the other slot. The board verifies that image,
        selects it and restarts; the owner-modified stock firmware then waits
        for the same confirmation an upload gets, which this sends. One switch
        request, never repeated after an ambiguous disconnect.
        """
        expected = sha256(expected)
        current = self.status()
        if current['running_sha256'] != expected:
            raise DeviceError('Firmware changed. Read firmware status before switching.', 409)
        if current['update_busy'] or current['pending_confirmation'] or current['running_slot'] != current['boot_slot']:
            raise DeviceError('This firmware is not confirmed yet; it returns to the other slot by itself.', 409)
        target = 'app0' if current['running_slot'] == 'app1' else 'app1'
        try:
            receipt = self._request('POST', expected=expected, path='/v1/firmware/switch', timeout=20)
        except DeviceError as e:
            if getattr(e, 'status', None) == 501:
                raise DeviceError('The installed firmware cannot switch slots (owned firmware only).', 501) from None
            raise
        other = receipt.get('target_sha256')
        if (receipt.get('switching') is not True or receipt.get('target_slot') != target
                or not isinstance(other, str) or not re.fullmatch('[0-9a-f]{64}', other)
                or receipt.get('rebooting') is not True):
            raise DeviceError('Switch receipt is invalid. Read firmware status; do not retry automatically.')
        deadline = time.monotonic() + POLL_SECONDS
        while time.monotonic() < deadline:
            try:
                status = self.status(timeout=max(0.1, min(3, deadline - time.monotonic())))
                if (status['running_sha256'] == other and status['running_slot'] == target
                        and status['boot_slot'] == target and not status['update_busy']):
                    if status['pending_confirmation']:
                        self._request('POST', expected=other, path='/v1/firmware/confirm', timeout=3)
                        status = self.status()
                    if not status['pending_confirmation']:
                        return {'switched': True, 'device_confirmed': True, 'target_slot': target,
                                'sha256': other, 'firmware': status}
            except DeviceError:
                pass  # restarting; the switch is not repeated
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(1, remaining))
        return {'switched': True, 'device_confirmed': False, 'target_slot': target, 'sha256': other,
                'message': 'Switch accepted, but the other firmware is not confirmed. Read firmware status.'}
