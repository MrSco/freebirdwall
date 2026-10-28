// The installer's steps over a device interface, so the same code runs on a
// real board (app.js wraps esptool-js) and on a simulated one in the tests:
//
//   dev = { chip, flashBytes,
//           readReg(addr) -> u32,
//           readFlash(addr, size, onProgress?) -> Uint8Array,
//           writeFlash(addr, bytes, onProgress?) }
//
// Order is chosen so a failure at any point leaves a board that starts a
// working firmware: the new image goes into a slot the next boot does not
// use, is read back and compared, and only then is the next boot changed.
// An update keeps the vendor firmware (targetSlot): it writes over the
// FreeBirdWall slot after pointing the next boot at the vendor firmware.
import {
  EFUSE_WORDS, FLASH_BYTES, OTADATA, PARTITION_TABLE, SLOTS, SLOT_BYTES,
  appDescription, checkBoard, decodeEfuses, hex, installedKey, newKey, otaBoot, otaSelect, packImage,
  parseImage, sha256, slotKind,
} from './rules.js';

export async function readFacts(dev) {
  const words = {};
  for (const [name, addr] of Object.entries(EFUSE_WORDS)) words[name] = (await dev.readReg(addr)) >>> 0;
  return {
    chip: dev.chip,
    flashBytes: dev.flashBytes,
    efuse: decodeEfuses(words),
    partitions: await dev.readFlash(PARTITION_TABLE, 0xc00),
    otadata: await dev.readFlash(OTADATA, 0x2000),
    heads: [await dev.readFlash(SLOTS[0], 0x1000), await dev.readFlash(SLOTS[1], 0x1000)],
  };
}

export async function check(dev) {
  return checkBoard(await readFacts(dev));
}

// The whole flash, for the owner to keep.
export async function backup(dev, onProgress) {
  const bytes = await dev.readFlash(0, FLASH_BYTES, onProgress);
  if (bytes.length !== FLASH_BYTES) throw new Error('the backup came back short');
  return { bytes, sha256: hex(await sha256(bytes)) };
}

// The application image in a slot, read just as far as it goes.
export async function readSlotImage(dev, slot) {
  const base = SLOTS[slot];
  const head = await dev.readFlash(base, 24);
  if (head[0] !== 0xe9 || head[1] < 1 || head[1] > 16) return null;
  let pos = 24;
  for (let i = 0; i < head[1]; ++i) {
    const seg = await dev.readFlash(base + pos, 8);
    const size = new DataView(seg.buffer, seg.byteOffset, 8).getUint32(4, true);
    if (!size || size % 4 || pos + 8 + size > SLOT_BYTES) return null;
    pos += 8 + size;
  }
  const length = Math.floor(pos / 16) * 16 + 16 + 32;
  if (length > SLOT_BYTES) return null;
  const image = await dev.readFlash(base, length);
  try { parseImage(image); } catch { return null; }
  return image;
}

// Where an install goes: the slot that is not running, except on an update
// whose other slot holds the vendor firmware. Then the update replaces the
// running FreeBirdWall, so the vendor firmware stays for "Start the vendor
// firmware again" however many updates follow.
export function targetSlot(report) {
  const other = 1 - report.bootSlot;
  return report.mode === 'update' && report.kinds[other] === 'vendor' ? report.bootSlot : other;
}

// Points the next boot at `slot`, and checks it took.
async function selectBoot(dev, slot) {
  const sel = otaSelect(await dev.readFlash(OTADATA, 0x2000), slot);
  await dev.writeFlash(sel.address, sel.sector);
  return otaBoot(await dev.readFlash(OTADATA, 0x2000)).slot === slot;
}

// Installs the owned firmware `base` (the unpackaged build) into targetSlot
// and makes it the next boot. A board already on the owned firmware keeps its
// key (Wi-Fi updates and sign-ins stay valid); a board on the vendor firmware
// gets a new random key.
export async function install(dev, report, base, onProgress = () => {}) {
  if (report.mode !== 'install' && report.mode !== 'update') throw new Error('the board check did not pass');
  const target = targetSlot(report);
  let key = null;
  if (report.mode === 'update') {
    onProgress('key', 0, 1);
    const running = await readSlotImage(dev, report.bootSlot);
    key = running && installedKey(running);
    if (!key) throw new Error('could not read the key of the firmware on the board');
  }
  key = key || newKey();
  const image = await packImage(base, key);
  let starts = 'its old firmware';              /* what the board starts if this stops here (error.starts) */
  try {
    if (target === report.bootSlot) {
      const vendor = 1 - target;
      if (slotKind(appDescription(await dev.readFlash(SLOTS[vendor], 0x1000))) !== 'vendor') throw new Error('the other slot changed; check the board again');
      if (!await selectBoot(dev, vendor)) throw new Error('the next start could not be changed');
      starts = 'the vendor firmware';
    }
    await dev.writeFlash(SLOTS[target], image, (done, total) => onProgress('write', done, total));
    const back = await dev.readFlash(SLOTS[target], image.length, (done, total) => onProgress('verify', done, total));
    if (hex(await sha256(back)) !== hex(await sha256(image))) throw new Error('the written firmware did not read back the same');
    if (!await selectBoot(dev, target)) throw new Error('the next start could not be selected');
  } catch (e) {
    e.starts = starts;
    throw e;
  }
  return { key, target, imageSha256: hex(await sha256(image)), desc: appDescription(image) };
}

// Starts the vendor firmware again (it stays in the other slot after an
// install, and after updates: targetSlot). Only the boot choice changes.
export async function switchToVendor(dev, report) {
  const slot = report.kinds.indexOf('vendor');
  if (slot < 0) throw new Error('no vendor firmware on the board; restore your backup instead');
  if (slot === report.bootSlot) return { slot, changed: false };
  const head = await dev.readFlash(SLOTS[slot], 0x1000);
  if (slotKind(appDescription(head)) !== 'vendor') throw new Error('the other slot changed; check the board again');
  if (!await selectBoot(dev, slot)) throw new Error('the next start could not be selected');
  return { slot, changed: true };
}

// Writes a whole-flash backup back. It must be this board's own backup: 8 MB,
// the same partition table, a FlightWall firmware in the slot it boots.
export async function restoreBackup(dev, bytes, onProgress = () => {}) {
  if (bytes.length !== FLASH_BYTES) throw new Error('a backup is exactly 8 MB; this file is not one');
  const backupReport = checkBoard({
    chip: dev.chip, flashBytes: dev.flashBytes, efuse: { octalFlash: false, psramCap: 0, flashEncryption: false, secureBoot: false, secureDownload: false },
    partitions: bytes.subarray(PARTITION_TABLE, PARTITION_TABLE + 0xc00), otadata: bytes.subarray(OTADATA, OTADATA + 0x2000),
    heads: SLOTS.map((a) => bytes.subarray(a, a + 0x1000)),
  });
  if (!backupReport.ok) throw new Error('this file is not a FlightWall backup: ' + backupReport.checks.filter((c) => !c.ok).map((c) => c.label).join('; '));
  const table = await dev.readFlash(PARTITION_TABLE, 0xc00);
  if (hex(table) !== hex(bytes.subarray(PARTITION_TABLE, PARTITION_TABLE + 0xc00))) throw new Error('this backup has a different memory layout from the board');
  await dev.writeFlash(0, bytes, (done, total) => onProgress('write', done, total));
  const back = await dev.readFlash(0, FLASH_BYTES, (done, total) => onProgress('verify', done, total));
  if (hex(await sha256(back)) !== hex(await sha256(bytes))) throw new Error('the restored flash did not read back the same');
  return { sha256: hex(await sha256(bytes)) };
}

// Status lines the owned firmware prints while it starts (wall_log.c), for
// the last step: did it pass its own board check, and where is the dashboard.
export function readStartLine(line, state) {
  const m = /^FWSTATE (\{.*\})\s*$/.exec(line.trim());
  if (/^FWPANIC /.test(line.trim())) state.panic = line.trim();
  if (!m) return state;
  let o;
  try { o = JSON.parse(m[1]); } catch { return state; }
  if ('board_ok' in o) { state.boardOk = o.board_ok; state.boardWhy = o.why || ''; }
  if (o.dashboard) state.dashboard = o.dashboard;
  if ('safe_mode' in o) state.safeMode = o.safe_mode;
  return state;
}
