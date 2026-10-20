// A pretend FlightWall for trying the page without a wall (installer.html#demo):
// a synthetic 8 MB flash with the FlightWall partition table, the vendor's
// firmware named in both slots (headers only, no vendor code) and app1
// selected, as on a wall from the box. Reads and writes take time, so the
// progress bars move. The first start prints what the firmware prints.
import { FLASH_BYTES, OTADATA, PARTITION_TABLE, SLOTS, EXPECTED_PARTITIONS, otaSelect } from './rules.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function vendorHead(version) {
  const h = new Uint8Array(0x1000).fill(0xff);
  h.set([0xe9, 1, 2, 0x2f], 0);
  new DataView(h.buffer).setUint16(12, 9, true);
  h[23] = 1;
  const v = new DataView(h.buffer);
  v.setUint32(24, 0x3c000020, true);
  v.setUint32(28, 256, true);
  v.setUint32(32, 0xabcd5432, true);
  const put = (at, s) => { h.fill(0, at, at + 32); h.set(new TextEncoder().encode(s), at); };
  put(48, version);
  put(80, 'arduino-lib-builder');
  put(112, '00:00:00');
  put(128, 'Mar  5 2024');
  put(144, 'v4.4.7');
  return h;
}

export function demoDevice() {
  const flash = new Uint8Array(FLASH_BYTES).fill(0xff);
  EXPECTED_PARTITIONS.forEach((p, i) => {
    const at = PARTITION_TABLE + i * 32, v = new DataView(flash.buffer);
    v.setUint16(at, 0x50aa, true);
    flash[at + 2] = p.type;
    flash[at + 3] = p.subtype;
    v.setUint32(at + 4, p.offset, true);
    v.setUint32(at + 8, p.size, true);
    flash.fill(0, at + 12, at + 32);
    flash.set(new TextEncoder().encode(p.label), at + 12);
  });
  flash.set(vendorHead('esp-idf: v4.4.7 38eeba213a'), SLOTS[0]);   // as a stock wall reports itself
  flash.set(vendorHead('esp-idf: v4.4.7 38eeba213a'), SLOTS[1]);
  const sel = otaSelect(flash.subarray(OTADATA, OTADATA + 0x2000), 1);
  flash.set(sel.sector, sel.address);
  const paced = async (size, onProgress, bytesPerMs) => {
    if (size < 65536 || !onProgress) return;
    for (let done = 0; done < size; done += 262144) { onProgress(Math.min(done, size), size); await sleep(262144 / bytesPerMs); }
    onProgress(size, size);
  };
  return {
    chip: 'ESP32-S3',
    flashBytes: FLASH_BYTES,
    readReg: async () => 0,
    readFlash: async (addr, size, onProgress) => { await paced(size, onProgress, 400); return flash.slice(addr, addr + size); },
    writeFlash: async (addr, bytes, onProgress) => { await paced(bytes.length, onProgress, 150); flash.set(bytes, addr); },
    // after a reset: what the owned firmware prints while it starts
    startLines: [
      '--- boot 1, reset reason 1 ---',
      'FWSTATE {"crashes_in_a_row":0,"safe_mode":false}',
      'FWSTATE {"board_ok":true,"why":"","flash_octal":0,"psram_cap":0,"flash_mb":8,"partitions":true,"model":"mini-v1"}',
      'FWMEM {"stage":"wifi_online"}',
      'FWSTATE {"dashboard":"http://192.168.1.50:8080/","paired":0}',
    ],
  };
}
