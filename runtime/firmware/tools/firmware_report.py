"""Offline size/memory report for the owned firmware build. No device access.

Run after `idf.py build` in runtime/firmware:

    py tools/firmware_report.py [--build build] [--out ../../analysis/source-firmware/size-report.json]

Reports what the linker actually kept: static DRAM/IRAM, flash code/rodata,
the per-library breakdown (our TLS vs Wi-Fi vs lwIP vs IDF's own mbedtls),
the TLS arena's share, image/partition fit, and that the image header matches
the installed stock image (DIO, 80 MHz, 8 MB, ESP32-S3). It also re-checks that
no unprefixed mbedtls/psa symbol escapes the private TLS library.

Static numbers are exact. "Heap at boot" here is an upper bound from the
linker's view of DRAM; the board's own FWMEM "boot" line is the real figure.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
REPO = FIRMWARE.parent.parent
APP_PARTITION_BYTES = 0x2C0000
EXPECTED_HEADER = {'chip_id': 9, 'flash_mode': 'DIO', 'flash_size': '8MB', 'flash_freq': '80m'}

GROUPS = [
    ('owned_tls', r'libfw_tls\.a'),
    ('owned_main', r'libmain\.a'),
    ('wifi_blobs', r'lib(net80211|pp|core|espnow|mesh|smartconfig|wapi|phy|rtc|btbb|coexist)\.a'),
    ('wifi_driver', r'lib(esp_wifi|esp_phy|wpa_supplicant)\.a'),
    ('idf_mbedtls', r'lib(mbedtls|mbedcrypto|mbedx509|mbedtls_2)\.a'),
    ('lwip_netif', r'lib(lwip|esp_netif|tcpip_adapter)\.a'),
    ('freertos', r'libfreertos\.a'),
    ('libc', r'lib(c|m|newlib|gcc|stdc\+\+|cxx)\.a'),
]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit('command failed: %s\n%s%s' % (' '.join(map(str, cmd)), r.stdout, r.stderr))
    return r.stdout


def idf_size(idf_path, map_file, *extra):
    out = run([sys.executable, str(Path(idf_path) / 'tools' / 'idf_size.py'), '--json', *extra, str(map_file)])
    return json.loads(out)


def group_of(archive):
    name = Path(archive).name
    for group, pattern in GROUPS:
        if re.fullmatch(pattern, name):
            return group
    return 'other_idf'


def header_of(image):
    data = image.read_bytes()[:24]
    modes = {0: 'QIO', 1: 'QOUT', 2: 'DIO', 3: 'DOUT'}
    sizes = {0: '1MB', 1: '2MB', 2: '4MB', 3: '8MB', 4: '16MB'}
    freqs = {0: '40m', 1: '26m', 2: '20m', 0xf: '80m'}
    return {'chip_id': int.from_bytes(data[12:14], 'little'),
            'flash_mode': modes.get(data[2], hex(data[2])),
            'flash_size': sizes.get(data[3] >> 4, hex(data[3] >> 4)),
            'flash_freq': freqs.get(data[3] & 0xF, hex(data[3] & 0xF))}


def symbol_size(nm, elf, name):
    for line in run([nm, '-S', '--defined-only', str(elf)]).splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] == name:
            return int(parts[1], 16)
    return None


def heap_region(nm, elf, build):
    """Exact size of the main heap region: from _heap_start (end of static
    data/bss) to the top of dram0_0_seg. On the S3, IRAM code is carved from the
    same SRAM, so idf_size's DIRAM remainder overstates this. ROM/bootloader
    memory reclaimed at startup adds to the heap; startup task stacks subtract."""
    ld = (build / 'esp-idf' / 'esp_system' / 'ld' / 'memory.ld').read_text()
    m = re.search(r'dram0_0_seg \(RW\) : org = (.+?), len = (.+?)$', ld, re.M)
    if not m:
        return None
    safe = re.compile(r'^[0-9a-fA-Fx()+\- ]+$')
    if not (safe.match(m.group(1)) and safe.match(m.group(2))):
        return None
    top = eval(m.group(1)) + eval(m.group(2))  # plain hex arithmetic, checked above
    start = None
    for line in run([nm, str(elf)]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == '_heap_start':
            start = int(parts[0], 16)
    return {'heap_start': hex(start), 'dram_top': hex(top), 'bytes': top - start} if start else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', type=Path, default=FIRMWARE / 'build')
    ap.add_argument('--out', type=Path, default=REPO / 'analysis' / 'source-firmware' / 'size-report.json')
    a = ap.parse_args()

    idf_path = os.environ.get('IDF_PATH')
    if not idf_path:
        raise SystemExit('IDF_PATH is not set; run inside the ESP-IDF v4.4.7 environment.')
    project = 'freebirdwall'
    elf, map_file, image = (a.build / (project + ext) for ext in ('.elf', '.map', '.bin'))
    for f in (elf, map_file, image):
        if not f.exists():
            raise SystemExit('missing %s; run idf.py build first' % f)

    totals = idf_size(idf_path, map_file)
    archives = idf_size(idf_path, map_file, '--archives')

    groups = {}
    for archive, sizes in archives.items():
        g = groups.setdefault(group_of(archive), {'archives': [], 'flash_text': 0, 'flash_rodata': 0,
                                                   'iram': 0, 'dram_data': 0, 'dram_bss': 0})
        g['archives'].append(Path(archive).name)
        # IDF v4.4.7 --archives keys are linker output section names.
        g['flash_text'] += sizes.get('.flash.text', 0)
        g['flash_rodata'] += sizes.get('.flash.rodata', 0)
        g['iram'] += sizes.get('.iram0.text', 0) + sizes.get('.iram0.vectors', 0)
        g['dram_data'] += sizes.get('.dram0.data', 0)
        g['dram_bss'] += sizes.get('.dram0.bss', 0)

    nm = os.environ.get('FW_NM', 'xtensa-esp32s3-elf-nm')
    arena = symbol_size(nm, elf, 's_tls_arena')
    context = symbol_size(nm, elf, 's_tls_context')

    tls_archive = next(a.build.rglob('libfw_tls.a'), None)
    leaked = []
    if tls_archive:
        for line in run([nm, '-g', '--defined-only', str(tls_archive)]).splitlines():
            m = re.match(r'^\s*[0-9a-fA-F]+\s+[A-Z]\s+(\w+)\s*$', line)
            if m and m.group(1).startswith(('mbedtls_', 'psa_')):
                leaked.append(m.group(1))

    heap = heap_region(nm, elf, a.build)
    header = header_of(image)
    image_bytes = image.stat().st_size
    report = {
        'project': project,
        'image_bytes': image_bytes,
        'app_partition_bytes': APP_PARTITION_BYTES,
        'image_fits_partition': image_bytes <= APP_PARTITION_BYTES,
        'image_partition_headroom': APP_PARTITION_BYTES - image_bytes,
        'image_header': header,
        'image_header_matches_stock': header == EXPECTED_HEADER,
        'idf_size_totals': totals,
        'main_heap_region': heap,
        'tls_arena_bytes': arena,
        'tls_context_reserved_bytes': context,
        'groups': groups,
        'tls_symbol_isolation_ok': tls_archive is not None and not leaked,
        'tls_symbols_leaked': leaked[:20],
        'notes': [
            'Static figures are exact linker output.',
            'idf_size DIRAM remainder ignores IRAM code sharing the same SRAM; main_heap_region is the exact linker figure.',
            'Runtime free heap (after reclaim, startup tasks, Wi-Fi) only comes from the board FWMEM lines.',
            'The TLS arena is .bss on purpose: it is reserved at boot and cannot be fragmented away.',
        ],
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2))

    # ESP32-S3: data RAM is reported as the shared DIRAM region in v4.4.7.
    used_dram = totals.get('used_diram') or totals.get('used_dram', 0)
    avail_dram = totals.get('diram_remain') or totals.get('dram_remain', 0)
    print('image            %8d B  (partition headroom %d B)' % (image_bytes, report['image_partition_headroom']))
    print('header           %s  matches stock: %s' % (header, report['image_header_matches_stock']))
    print('static DRAM used %8d B  (data %d + bss %d), linker remainder %d B'
          % (used_dram, totals.get('diram_data', 0), totals.get('diram_bss', 0), avail_dram))
    print('  of which TLS arena %s B, TLS context %s B' % (arena, context))
    if heap:
        print('main heap region %8d B  (%s..%s, exact; before startup reclaim/tasks)'
              % (heap['bytes'], heap['heap_start'], heap['dram_top']))
    print('IRAM code        %8d B  (runs from internal RAM: ISRs, Wi-Fi hot paths)' % totals.get('used_iram', 0))
    print('flash code       %8d B  rodata %d B' % (totals.get('flash_code', 0), totals.get('flash_rodata', 0)))
    print('by group          flash text   rodata     IRAM  DRAM data+bss')
    for name, g in sorted(groups.items(), key=lambda kv: -(kv[1]['flash_text'] + kv[1]['flash_rodata'])):
        print('  %-12s %12d %8d %8d %8d' % (name, g['flash_text'], g['flash_rodata'], g['iram'],
                                            g['dram_data'] + g['dram_bss']))
    print('TLS symbol isolation: %s' % ('ok' if report['tls_symbol_isolation_ok'] else 'FAILED %s' % leaked[:5]))
    print('report -> %s' % a.out)
    ok = report['image_fits_partition'] and report['image_header_matches_stock'] and report['tls_symbol_isolation_ok']
    sys.exit(0 if ok else 2)


if __name__ == '__main__':
    main()
