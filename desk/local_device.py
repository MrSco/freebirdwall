"""Direct LAN owner API. No DNS, proxies, redirects, cloud, or credential logging."""
from __future__ import annotations

import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import tempfile
import time

PORT = 8787
TIMEOUT = 3
MAX_BODY = 16384
MAX_REVISION = 0xffffffff
_NETWORKS = tuple(ipaddress.IPv4Network(n) for n in
                  ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16'))


class DeviceError(ValueError):
    def __init__(self, message, status=502):
        super().__init__(message)
        self.status = status


def address(value):
    try:
        ip = ipaddress.IPv4Address(value) if isinstance(value, str) else None
    except ipaddress.AddressValueError:
        ip = None
    if ip is None or not any(ip in network for network in _NETWORKS):
        raise DeviceError('Use a private or link-local IPv4 address for the device.', 400)
    if any(ip == net.network_address or ip == net.broadcast_address
           for net in _NETWORKS):
        raise DeviceError('Use the device IPv4 address, not a network or broadcast address.', 400)
    return str(ip)


def credentials(ip, token):
    ip = address(ip)
    if not isinstance(token, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', token):
        raise DeviceError('The device key must contain 64 hexadecimal characters.', 400)
    return {'ip': ip, 'token': token.lower()}


def load_connection(path):
    path = Path(path)
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        return credentials(data['ip'], data['token'])
    except (OSError, ValueError, KeyError, TypeError):
        raise DeviceError('Cannot read local device connection settings. Reconnect the device.', 400) from None


def save_connection(path, connection):
    """Atomic private file; the key never appears in public API responses."""
    connection = credentials(connection.get('ip'), connection.get('token'))
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=Path(path).parent,
                                         delete=False) as handle:
            temporary = Path(handle.name)
            os.chmod(temporary, 0o600)
            json.dump(connection, handle)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except OSError:
        raise DeviceError('Could not save local connection settings on this PC.', 500) from None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def revision(value):
    if type(value) is not int or not 0 <= value <= MAX_REVISION:
        raise DeviceError('Device returned an invalid alias revision.')
    return value


class Client:
    def __init__(self, connection):
        if connection is None:
            raise DeviceError('No local device configured. Enter its LAN address and device key.', 400)
        self.connection = credentials(connection.get('ip'), connection.get('token'))

    def _raw_request(self, method, path, data=None, extra_headers=None):
        if data is not None and len(data) > MAX_BODY:
            raise DeviceError('Command is too large.', 400)
        headers = {'Authorization': 'Bearer ' + self.connection['token'],
                   'Accept': 'application/json', 'Connection': 'close'}
        if data is not None:
            headers['Content-Type'] = 'application/json'
        if extra_headers:
            headers.update(extra_headers)
        conn = None
        try:
            conn = http.client.HTTPConnection(self.connection['ip'], PORT, timeout=TIMEOUT)
            conn.request(method, path, body=data, headers=headers)
            response = conn.getresponse()
            if response.status != 200:
                if response.status == 409 or response.status == 412:
                    raise DeviceError('Device renames changed. Read installed rules before applying again.', 409)
                if response.status in (401, 403):
                    raise DeviceError('Device key was rejected. Reconnect using the provisioned key.', 401)
                if 300 <= response.status < 400:
                    raise DeviceError('Device redirected the request. Redirects are disabled.')
                raise DeviceError('Device rejected the request (HTTP %d). No save confirmed.' % response.status,
                                  response.status if response.status in (400, 413, 415, 422, 503) else 502)
            length = response.getheader('Content-Length')
            if length is not None and (not length.isdigit() or int(length) > MAX_BODY):
                raise DeviceError('Device response is too large or malformed.')
            deadline = time.monotonic() + TIMEOUT
            chunks = bytearray()
            while len(chunks) <= MAX_BODY:
                if time.monotonic() > deadline:
                    raise DeviceError('Local device response timed out. No save confirmed.')
                chunk = response.read1(min(2048, MAX_BODY + 1 - len(chunks)))
                if not chunk:
                    break
                chunks.extend(chunk)
            if len(chunks) > MAX_BODY:
                raise DeviceError('Device response is too large.')
            body = json.loads(chunks.decode('utf-8'))
            if not isinstance(body, dict):
                raise DeviceError('Device returned an unsupported response.')
            return body, response.getheader('ETag')
        except DeviceError:
            raise
        except (OSError, http.client.HTTPException, UnicodeError, ValueError, TypeError):
            raise DeviceError('Local device is offline or returned an invalid response. No save confirmed.') from None
        finally:
            if conn is not None:
                conn.close()

    def _request(self, method, path, body=None, expected=None):
        # This is intentionally HTTP over the trusted local network. http.client
        # uses the validated literal address directly and ignores proxy variables.
        data = None if body is None else json.dumps(body, separators=(',', ':')).encode('utf-8')
        if data is not None and len(data) > MAX_BODY:
            raise DeviceError('Alias command is too large.', 400)
        extra = {'If-Match': '"%d"' % revision(expected)} if expected is not None else None
        parsed, etag = self._raw_request(method, path, data=data, extra_headers=extra)
        if (type(parsed.get('schema_version')) is not int or parsed.get('schema_version') != 1
                or type(parsed.get('api_version')) is not int or parsed.get('api_version') != 1):
            raise DeviceError('Device returned an unsupported response.')
        current = revision(parsed.get('revision'))
        if etag != '"%d"' % current:
            raise DeviceError('Device returned an invalid revision tag.')
        return parsed

    def status(self):
        body = self._request('GET', '/v1/status')
        count = body.get('alias_count')
        if type(count) is not int or not 0 <= count <= 16:
            raise DeviceError('Device returned an invalid alias count.')
        if type(body.get('storage_ready')) is not bool or body.get('storage') != 'nvs':
            raise DeviceError('Device returned an invalid storage status.')
        return {'schema_version': 1, 'revision': body['revision'], 'alias_count': count,
                'storage_ready': body['storage_ready'], 'connected': True,
                'ip': self.connection['ip'], 'device_confirmed': True}

    def aliases(self, clean_alias_list):
        body = self._request('GET', '/v1/aliases')
        try:
            rows = clean_alias_list(body.get('aliases'))
        except ValueError:
            raise DeviceError('Device returned invalid rename rules.') from None
        return {'schema_version': 1, 'revision': body['revision'], 'aliases': rows,
                'device_confirmed': True}

    def apply(self, rows, expected, clean_alias_list):
        if type(expected) is not int or not 0 <= expected <= MAX_REVISION:
            raise DeviceError('Read the installed device revision before applying rules.', 400)
        if expected == MAX_REVISION:
            raise DeviceError('Device revision exhausted. Export rules before migrating storage.', 409)
        # Require a fresh read as well as If-Match. A stale browser can never
        # silently overwrite rules installed from another window or client.
        current = self.aliases(clean_alias_list)
        if current['revision'] != expected:
            raise DeviceError('Device renames changed. Read installed rules before applying again.', 409)
        command = {'schema_version': 1, 'revision': expected + 1, 'aliases': clean_alias_list(rows)}
        ack = self._request('PUT', '/v1/aliases', {'owner_aliases': command}, expected)
        if ack.get('aliases') != command['aliases'] or ack['revision'] != command['revision']:
            raise DeviceError('Device did not confirm the exact saved rules. Read installed rules before retrying.')
        return {'schema_version': 1, 'revision': ack['revision'], 'aliases': command['aliases'],
                'alias_count': len(command['aliases']), 'device_confirmed': True}

    def settings(self):
        body, _ = self._raw_request('GET', '/v1/settings')
        if not isinstance(body.get('display_config'), dict) or not isinstance(body.get('request_config'), dict):
            raise DeviceError('Device returned an invalid settings document.')
        return body

    def save_settings(self, doc):
        if not isinstance(doc, dict) or not isinstance(doc.get('display_config'), dict) or not isinstance(doc.get('request_config'), dict):
            raise DeviceError('Settings require display_config and request_config objects.', 400)
        data = json.dumps(doc, separators=(',', ':')).encode('utf-8')
        if len(data) > 4095:
            raise DeviceError('Settings JSON exceeds the 4095-byte NVS limit.', 413)
        origin = f'http://{self.connection["ip"]}:{PORT}'
        body, _ = self._raw_request('PUT', '/v1/settings', data=data, extra_headers={'Origin': origin})
        if not isinstance(body.get('display_config'), dict) or not isinstance(body.get('request_config'), dict):
            raise DeviceError('Device did not confirm the saved settings.')
        return body
