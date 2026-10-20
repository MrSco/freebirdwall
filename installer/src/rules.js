// The FlightWall installer's own rules: what a compatible board looks like,
// how the owned firmware is packaged for one board, and how the next boot is
// chosen. No serial code here (app.js drives esptool-js); everything takes and
// returns bytes, so tests/test_installer.mjs runs it against a real flash image.
//
// Mirrors the firmware's board check (runtime/firmware/main/
// board_check.c), the packager (tools/package_owned.py) and ESP-IDF's otadata
// format, so the browser refuses exactly what the firmware would refuse.

export const FLASH_BYTES = 8 * 1024 * 1024;
export const SLOT_BYTES = 0x2c0000;
export const SLOTS = [0x10000, 0x2d0000];          // app0, app1
export const OTADATA = 0xe000;                     // two 4 KiB sectors
export const PARTITION_TABLE = 0x8000;
export const VENDOR_PROJECT = 'arduino-lib-builder';
export const OWNED_PROJECTS = ['freebirdwall', 'flightwall-owned'];   // and its name before the rename

// The FlightWall mini-v1 partition table, exactly (board_check.c TABLE).
export const EXPECTED_PARTITIONS = [
  { label: 'nvs', type: 1, subtype: 0x02, offset: 0x9000, size: 0x5000 },
  { label: 'otadata', type: 1, subtype: 0x00, offset: 0xe000, size: 0x2000 },
  { label: 'app0', type: 0, subtype: 0x10, offset: 0x10000, size: 0x2c0000 },
  { label: 'app1', type: 0, subtype: 0x11, offset: 0x2d0000, size: 0x2c0000 },
  { label: 'spiffs', type: 1, subtype: 0x82, offset: 0x590000, size: 0x270000 },
];

// ESP32-S3 eFuse words read through the ROM loader (DR_REG_EFUSE_BASE
// 0x60007000; ESP-IDF esp32s3 esp_efuse_table.csv bit numbers).
export const EFUSE_WORDS = {
  blk0_w2: 0x60007034,   // BLK0 bits 64..95: SPI_BOOT_CRYPT_CNT (82..84)
  blk0_w3: 0x60007038,   // BLK0 bits 96..127: SECURE_BOOT_EN (116)
  blk0_w4: 0x6000703c,   // BLK0 bits 128..159: DIS_DOWNLOAD_MODE (128), ENABLE_SECURITY_DOWNLOAD (133), FLASH_TYPE (137)
  blk1_w4: 0x60007054,   // BLK1 bits 128..159: PSRAM_CAP (131..132)
};

const bit = (word, n) => (word >>> n) & 1;
const text = (bytes) => new TextDecoder().decode(bytes).split('\0', 1)[0];
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

export function decodeEfuses(w) {
  const crypt = (w.blk0_w2 >>> 18) & 7;
  return {
    flashEncryption: ((crypt & 1) + ((crypt >> 1) & 1) + ((crypt >> 2) & 1)) % 2 === 1,   // an odd count: on
    secureBoot: bit(w.blk0_w3, 20) === 1,
    downloadDisabled: bit(w.blk0_w4, 0) === 1,
    secureDownload: bit(w.blk0_w4, 5) === 1,
    octalFlash: bit(w.blk0_w4, 9) === 1,
    psramCap: (w.blk1_w4 >>> 3) & 3,      // 0 none, 1 8 MB octal, 2 2 MB quad
  };
}

export function parsePartitions(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out = [];
  for (let at = 0; at + 32 <= bytes.length; at += 32) {
    const magic = v.getUint16(at, true);
    if (magic === 0xebeb || magic === 0xffff) break;        // MD5 entry or end
    if (magic !== 0x50aa) return null;
    out.push({
      type: bytes[at + 2], subtype: bytes[at + 3], offset: v.getUint32(at + 4, true), size: v.getUint32(at + 8, true),
      label: text(bytes.subarray(at + 12, at + 28)),
    });
  }
  return out;
}

export function partitionsMatch(parts) {
  return !!parts && parts.length === EXPECTED_PARTITIONS.length &&
    EXPECTED_PARTITIONS.every((e, i) => ['label', 'type', 'subtype', 'offset', 'size'].every((k) => parts[i][k] === e[k]));
}

// The app description at the start of an application image (esp_app_desc_t,
// after the 24-byte image header and the first 8-byte segment header).
export function appDescription(head) {
  if (head.length < 32 + 176 || head[0] !== 0xe9) return null;
  const v = new DataView(head.buffer, head.byteOffset, head.byteLength);
  if (v.getUint32(32, true) !== 0xabcd5432) return null;
  return {
    version: text(head.subarray(48, 80)), project: text(head.subarray(80, 112)),
    time: text(head.subarray(112, 128)), date: text(head.subarray(128, 144)), idf: text(head.subarray(144, 176)),
  };
}

export function slotKind(desc) {
  if (!desc) return 'empty';
  if (desc.project === VENDOR_PROJECT) return 'vendor';
  if (OWNED_PROJECTS.includes(desc.project)) return 'owned';
  return 'unknown';
}

// ---------------------------------------------------------------- otadata

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

// esp_rom_crc32_le(UINT32_MAX, buf, len) as the bootloader checks it.
export function otaCrc(bytes) {
  let c = 0;                                  // ~UINT32_MAX
  for (const b of bytes) c = (CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8)) >>> 0;
  return (~c) >>> 0;
}

export const OTA_VALID = 2;
export const OTA_UNDEFINED = 0xffffffff;
const OTA_INVALID_STATES = new Set([3, 4]);   // ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED

function readEntry(sector) {
  const v = new DataView(sector.buffer, sector.byteOffset, 32);
  const seq = v.getUint32(0, true), state = v.getUint32(24, true), crc = v.getUint32(28, true);
  const seqBytes = sector.subarray(0, 4);
  const ok = seq !== 0xffffffff && crc === otaCrc(seqBytes);
  return { seq, state, ok };
}

// Which slot the bootloader starts (two OTA slots): the valid entry with the
// highest sequence; none valid means app0 (the factory choice on this table).
export function otaBoot(otadata) {
  const entries = [readEntry(otadata.subarray(0, 32)), readEntry(otadata.subarray(4096, 4128))];
  let best = -1;
  entries.forEach((e, i) => {
    if (e.ok && !OTA_INVALID_STATES.has(e.state) && (best < 0 || e.seq > entries[best].seq)) best = i;
  });
  if (best < 0) return { slot: 0, sector: -1, seq: 0, entries };
  return { slot: (entries[best].seq - 1) % 2, sector: best, seq: entries[best].seq, entries };
}

// The otadata sector to write so the next boot is `slot`: the entry that is
// not the current one gets the next sequence that maps to the slot, state
// VALID (a USB install is checked by watching it start; see app.js).
export function otaSelect(otadata, slot) {
  const boot = otaBoot(otadata);
  let seq = Math.max(boot.entries[0].ok ? boot.entries[0].seq : 0, boot.entries[1].ok ? boot.entries[1].seq : 0) + 1;
  if ((seq - 1) % 2 !== slot) seq += 1;
  const sectorIndex = boot.sector === 0 ? 1 : 0;
  const sector = new Uint8Array(4096).fill(0xff);
  const v = new DataView(sector.buffer);
  v.setUint32(0, seq, true);
  v.setUint32(24, OTA_VALID, true);
  v.setUint32(28, otaCrc(sector.subarray(0, 4)), true);
  return { address: OTADATA + sectorIndex * 4096, sector, seq };
}

// ---------------------------------------------------------------- images

const DESCRIPTOR_MAGIC = [0x46, 0x57, 0x4f, 0x54, 0x41, 0x31, 0, 0];   // "FWOTA1\0\0"
const DESCRIPTOR_BYTES = 44;

export function parseImage(image) {
  if (image.length < 24 + 8 + 33 || image.length > SLOT_BYTES) throw new Error('image size is outside the app slot');
  if (image[0] !== 0xe9 || image[1] < 1 || image[1] > 16) throw new Error('not an ESP application image');
  const v = new DataView(image.buffer, image.byteOffset, image.byteLength);
  if (v.getUint16(12, true) !== 9) throw new Error('image is not built for the ESP32-S3');
  if (image[23] !== 1) throw new Error('image has no appended SHA-256');
  let pos = 24;
  const segments = [];
  for (let i = 0; i < image[1]; ++i) {
    if (pos + 8 > image.length) throw new Error('segment header truncated');
    const size = v.getUint32(pos + 4, true);
    pos += 8;
    if (!size || size % 4 || pos + size > image.length) throw new Error('segment bounds invalid');
    segments.push({ offset: pos, size });
    pos += size;
  }
  const footer = Math.floor(pos / 16) * 16 + 15;
  if (image.length !== footer + 33) throw new Error('image length invalid');
  for (let i = pos; i < footer; ++i) if (image[i]) throw new Error('image padding invalid');
  return { segments, footer };
}

function checksum(image, segments) {
  let c = 0xef;
  for (const s of segments) for (let i = s.offset; i < s.offset + s.size; ++i) c ^= image[i];
  return c;
}

async function sha256(bytes) {
  return new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
}

export async function verifyImage(image) {
  const { segments, footer } = parseImage(image);
  if (image[footer] !== checksum(image, segments)) throw new Error('image checksum byte is wrong');
  const digest = await sha256(image.subarray(0, footer + 1));
  if (hex(digest) !== hex(image.subarray(footer + 1))) throw new Error('image SHA-256 is wrong');
  return { segments, footer };
}

function descriptorHits(image) {
  const hits = [];
  outer: for (let at = image.indexOf(0x46); at >= 0 && at + 12 <= image.length; at = image.indexOf(0x46, at + 1)) {
    for (let k = 0; k < 8; ++k) if (image[at + k] !== DESCRIPTOR_MAGIC[k]) continue outer;
    const version = new DataView(image.buffer, image.byteOffset + at + 8, 4).getUint32(0, true);
    if (version === 0 || version === 1) hits.push({ at, version });
  }
  return hits;
}

// The device key of an installed owned image (its descriptor), or null.
export function installedKey(image) {
  const hits = descriptorHits(image).filter((h) => h.version === 1);
  if (hits.length !== 1) return null;
  const key = image.slice(hits[0].at + 12, hits[0].at + DESCRIPTOR_BYTES);
  return key.some((b) => b) ? key : null;
}

// The owned firmware for one board: its 32-byte key in the single empty
// descriptor, then the checksum byte and SHA-256 again (package_owned.py pack).
export async function packImage(base, key) {
  if (key.length !== 32 || !key.some((b) => b)) throw new Error('device key must be 32 non-zero bytes');
  const { segments } = await verifyImage(base);
  const hits = descriptorHits(base);
  if (hits.length !== 1) throw new Error(hits.length ? 'descriptor is not unique' : 'no owner descriptor in the image');
  const { at, version } = hits[0];
  if (version !== 0 || base.subarray(at + 12, at + DESCRIPTOR_BYTES).some((b) => b)) throw new Error('image is already packaged');
  if (!segments.some((s) => s.offset <= at && at + DESCRIPTOR_BYTES <= s.offset + s.size)) throw new Error('descriptor straddles a segment');
  const out = base.slice();
  new DataView(out.buffer).setUint32(at + 8, 1, true);
  out.set(key, at + 12);
  const { footer } = parseImage(out);
  out[footer] = checksum(out, segments);
  out.set(await sha256(out.subarray(0, footer + 1)), footer + 1);
  await verifyImage(out);
  return out;
}

export function newKey() {
  const k = new Uint8Array(32);
  do crypto.getRandomValues(k); while (!k.some((b) => b));
  return k;
}

export { hex, sha256 };

// ---------------------------------------------------------------- the board check

// Every check the installer makes before it writes anything. `facts`:
// { chip, flashBytes, efuse (decodeEfuses), partitions (bytes), otadata
// (bytes), heads: [app0 head, app1 head] (first 4 KiB of each slot) }.
export function checkBoard(facts) {
  const checks = [];
  const add = (id, label, ok, detail) => checks.push({ id, label, ok, detail });
  add('chip', 'Microcontroller is an ESP32-S3', facts.chip === 'ESP32-S3', facts.chip || 'unknown');
  add('flash', 'Flash memory is 8 MB', facts.flashBytes === FLASH_BYTES, `${(facts.flashBytes / 1048576) || '?'} MB`);
  const e = facts.efuse;
  add('octal', 'Flash and PSRAM leave the display pins free', !!e && !e.octalFlash && e.psramCap !== 1,
    !e ? 'not read' : e.octalFlash ? 'octal flash uses GPIO 33-37' : e.psramCap === 1 ? 'octal PSRAM uses GPIO 33-37' : 'quad flash, no octal PSRAM');
  add('security', 'No flash encryption or secure boot', !!e && !e.flashEncryption && !e.secureBoot && !e.secureDownload,
    !e ? 'not read' : e.flashEncryption ? 'flash encryption is on' : e.secureBoot ? 'secure boot is on' : e.secureDownload ? 'secure download mode is on' : 'off');
  const parts = facts.partitions ? parsePartitions(facts.partitions) : null;
  add('partitions', 'Memory layout is the FlightWall mini’s', partitionsMatch(parts),
    parts ? parts.map((p) => p.label).join(', ') : 'unreadable');
  const boot = facts.otadata ? otaBoot(facts.otadata) : null;
  const descs = (facts.heads || []).map(appDescription);
  const kinds = descs.map(slotKind);
  const running = boot ? kinds[boot.slot] : 'unknown';
  add('firmware', 'Runs the vendor firmware or FreeBirdWall', running === 'vendor' || running === 'owned',
    boot ? `slot ${boot.slot}: ${running}${descs[boot.slot] ? ' ' + descs[boot.slot].version : ''}` : 'boot slot unreadable');
  const ok = checks.every((c) => c.ok);
  return { ok, checks, bootSlot: boot ? boot.slot : -1, kinds, descs, mode: !ok ? 'stop' : running === 'owned' ? 'update' : 'install' };
}
