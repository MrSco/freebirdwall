"""Passive USB console capture for the owned-firmware probe run.

    py tools/capture_console.py --port COM5 [--seconds 300]

Start it BEFORE uploading the candidate. It only listens: it never writes to
the port and never toggles DTR/RTS, because a chip reset while the probe image
is pending would roll it back before it measured anything. The USB-Serial/JTAG
device disappears on every restart (upload -> probe boot -> rollback boot), so
the capture reopens the port whenever it comes back.

The raw log goes to analysis/source-firmware/ (private, git-ignored) and is
summarised with parse_probe_log.py when the capture ends. Stop early with Ctrl+C;
whatever was captured is still saved and summarised.
"""
import argparse
import datetime
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(HERE))
import parse_probe_log  # noqa: E402


def open_quietly(serial_module, port):
    """Open without asserting DTR/RTS (either can reset an ESP32-S3)."""
    handle = serial_module.Serial(port=None, baudrate=115200, timeout=0.25)
    handle.dtr = False
    handle.rts = False
    handle.port = port
    handle.open()
    return handle


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--seconds', type=int, default=300)
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    try:
        import serial
    except ImportError:
        sys.exit('pyserial is required: pip install pyserial')

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = a.out or REPO / 'analysis' / 'source-firmware' / ('console-%s.log' % stamp)
    out.parent.mkdir(parents=True, exist_ok=True)
    chunks, handle, reopens, pending = [], None, 0, b''
    end = time.monotonic() + a.seconds
    print('Listening on %s for %d s (no writes, no resets). Upload the candidate now.' % (a.port, a.seconds))
    try:
        while time.monotonic() < end:
            if handle is None:
                try:
                    handle = open_quietly(serial, a.port)
                    reopens += 1
                    print('  port open (%d)' % reopens)
                except (serial.SerialException, OSError):
                    time.sleep(0.5)
                    continue
            try:
                data = handle.read(max(handle.in_waiting, 1))
            except (serial.SerialException, OSError):
                handle.close()
                handle = None
                print('  port gone (board restarting)')
                continue
            if data:
                chunks.append(data)
                # USB reads split lines arbitrarily: echo only complete lines.
                pending += data
                *lines, pending = pending.split(b'\n')
                for line in lines:
                    text = line.decode('utf-8', 'replace').strip('\r')
                    if text.startswith(('FWSTATE', 'FWMEM')):
                        print('  ' + text[:160])
    except KeyboardInterrupt:
        print('stopped early')
    finally:
        if handle is not None:
            handle.close()

    raw = b''.join(chunks)
    out.write_bytes(raw)
    print('\nSaved %d bytes to %s' % (len(raw), out))
    mem, state = parse_probe_log.parse(raw.decode('utf-8', 'replace'))
    if not mem:
        print('No FWMEM lines captured. Was the port open before the probe image booted?')
        return 1
    for key, value in parse_probe_log.summarise(mem, state).items():
        if key not in ('state', 'final'):
            print('%-28s %s' % (key, value))
    return 0


if __name__ == '__main__':
    sys.exit(main())
