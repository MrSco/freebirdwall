"""Owner firmware commands from the desk, over the LAN.

    py desk/firmware_cli.py status
    py desk/firmware_cli.py switch    # boot the other slot (owned -> stock), then confirm it
    py desk/firmware_cli.py log       # the owned firmware's recent status lines, over Wi-Fi

Uses device.json (address and key) like the desk; the key is never printed.
Uploads stay in the desk UI (Firmware updates · Wi-Fi).
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import firmware_device  # noqa: E402
import local_device  # noqa: E402


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('status', 'switch', 'log'):
        sys.exit(__doc__)
    conn = local_device.load_connection(str(Path(__file__).resolve().parent / 'device.json'))
    client = firmware_device.Client(conn)
    try:
        if sys.argv[1] == 'log':
            print(client.log(), end='')
            return
        status = client.status()
        if sys.argv[1] == 'status':
            print(json.dumps(status, indent=1))
            return
        print('running %s in %s; switching to the other slot...' % (status['running_sha256'][:12], status['running_slot']))
        result = client.switch_slot(status['running_sha256'])
        result.pop('firmware', None)
        print(json.dumps(result, indent=1))
    except firmware_device.DeviceError as e:
        sys.exit('refused: %s' % e)


if __name__ == '__main__':
    main()
