"""Owner firmware commands from the desk, over the LAN.

    py desk/firmware_cli.py status
    py desk/firmware_cli.py log       # the owned firmware's recent status lines, over Wi-Fi
    py desk/firmware_cli.py update <candidate.bin>   # a packaged FreeBirdWall build (tools/package_owned.py)
    py desk/firmware_cli.py spare <vendor app .bin>  # the vendor firmware, unchanged, into the slot not running
    py desk/firmware_cli.py switch    # boot the other slot (owned -> stock), then confirm it

Uses desk/device.json (address and key), or --device <path> before the
command; the key is never printed. An update goes into the slot not
running, so it replaces a vendor firmware kept there (status: spare_slot);
put it back with `spare` once the updates are done, one write instead of
one per update (UPDATES.md "The vendor firmware in the spare slot").
Tests: py -m unittest discover desk
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import firmware_device  # noqa: E402
import local_device  # noqa: E402


def main():
    args = sys.argv[1:]
    device = Path(__file__).resolve().parent / 'device.json'
    if args[:1] == ['--device'] and len(args) > 1:
        device, args = Path(args[1]), args[2:]
    if not (len(args) == 1 and args[0] in ('status', 'switch', 'log') or
            len(args) == 2 and args[0] in ('update', 'spare')):
        sys.exit(__doc__)
    try:
        client = firmware_device.Client(local_device.load_connection(device))
        if args[0] == 'log':
            print(client.log(), end='')
            return
        status = client.status()
        if args[0] == 'status':
            print(json.dumps(status, indent=1))
            return
        if args[0] in ('update', 'spare'):
            image = Path(args[1]).read_bytes()
            print('running %s in %s; uploading...' % (status['running_sha256'][:12], status['running_slot']), flush=True)
            send = client.update if args[0] == 'update' else client.store_spare
            result = send(image, status['running_sha256'])
        else:
            print('running %s in %s; switching to the other slot...' % (status['running_sha256'][:12], status['running_slot']))
            result = client.switch_slot(status['running_sha256'])
        result.pop('firmware', None)
        print(json.dumps(result, indent=1))
        if result.get('replaced_stock'):
            print('The stock firmware that was in %s is replaced. When the updates are done, put it back:' %
                  result['target_slot'])
            print('    py desk/firmware_cli.py spare <vendor app .bin>')
    except firmware_device.DeviceError as e:
        sys.exit('refused: %s' % e)


if __name__ == '__main__':
    main()
