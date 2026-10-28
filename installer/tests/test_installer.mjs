// The installer's rules and steps on a simulated board. Needs Node 18+.
//
//   node installer/tests/test_installer.mjs [--backup <8 MB flash image>] [--image <owned build>]
//
// The backup is a real stock FlightWall flash (private, never committed);
// without one the flash-based tests are skipped. The image is the unpackaged
// owned build (runtime/firmware/build-keep/freebirdwall.bin).
// Packaging is also compared byte for byte with tools/package_owned.py.
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  EFUSE_WORDS, FLASH_BYTES, OTADATA, SLOTS, appDescription, checkBoard, decodeEfuses, hex, installedKey,
  otaBoot, otaCrc, otaSelect, packImage, parsePartitions, partitionsMatch, slotKind,
} from '../src/rules.js';
import { backup, check, install, readStartLine, readSlotImage, restoreBackup, switchToVendor } from '../src/flow.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..');
const arg = (name, dflt) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : dflt; };
const BACKUP = arg('--backup', join(REPO, 'backups', 'stock-original-2026-09-08-backup.bin'));
const IMAGE = arg('--image', join(REPO, 'runtime', 'firmware', 'build-keep', 'freebirdwall.bin'));

let checks = 0, failed = 0;
function ok(cond, what) { ++checks; if (!cond) { ++failed; console.log('FAIL', what); } }
async function throws(fn, pattern, what) {
  try { await fn(); ok(false, what + ' (did not throw)'); } catch (e) { ok(pattern.test(e.message), what + ': ' + e.message); }
}

// A board in a byte array, with eFuse words.
class FakeDevice {
  constructor(flash, { chip = 'ESP32-S3', efuse = {}, corruptWrites = false } = {}) {
    this.flash = flash; this.chip = chip; this.flashBytes = flash.length; this.corruptWrites = corruptWrites;
    this.efuse = { [EFUSE_WORDS.blk0_w2]: 0, [EFUSE_WORDS.blk0_w3]: 0, [EFUSE_WORDS.blk0_w4]: 0, [EFUSE_WORDS.blk1_w4]: 0, ...efuse };
    this.writes = [];
  }
  async readReg(addr) { if (!(addr in this.efuse)) throw new Error('unexpected register ' + addr.toString(16)); return this.efuse[addr]; }
  async readFlash(addr, size, onProgress) { onProgress && onProgress(size, size); return this.flash.slice(addr, addr + size); }
  async writeFlash(addr, bytes, onProgress) {
    if (addr % 4096) throw new Error('unaligned write');
    this.writes.push({ addr, size: bytes.length });
    this.flash.set(bytes, addr);
    if (this.corruptWrites) this.flash[addr + 100] ^= 0xff;
    onProgress && onProgress(bytes.length, bytes.length);
  }
}

// ---------------------------------------------------------------- pure rules
{
  const e = decodeEfuses({ blk0_w2: 0, blk0_w3: 0, blk0_w4: 0, blk1_w4: 0 });
  ok(!e.octalFlash && !e.flashEncryption && !e.secureBoot && e.psramCap === 0, 'efuse: a blank board');
  ok(decodeEfuses({ blk0_w2: 1 << 18, blk0_w3: 0, blk0_w4: 0, blk1_w4: 0 }).flashEncryption, 'efuse: one CRYPT_CNT bit = encrypted');
  ok(!decodeEfuses({ blk0_w2: 3 << 18, blk0_w3: 0, blk0_w4: 0, blk1_w4: 0 }).flashEncryption, 'efuse: two bits = off again');
  ok(decodeEfuses({ blk0_w2: 7 << 18, blk0_w3: 0, blk0_w4: 0, blk1_w4: 0 }).flashEncryption, 'efuse: three bits = on');
  ok(decodeEfuses({ blk0_w2: 0, blk0_w3: 1 << 20, blk0_w4: 0, blk1_w4: 0 }).secureBoot, 'efuse: secure boot (bit 116)');
  ok(decodeEfuses({ blk0_w2: 0, blk0_w3: 0, blk0_w4: 1 << 9, blk1_w4: 0 }).octalFlash, 'efuse: octal flash (bit 137)');
  ok(decodeEfuses({ blk0_w2: 0, blk0_w3: 0, blk0_w4: 0, blk1_w4: 1 << 3 }).psramCap === 1, 'efuse: PSRAM_CAP (BLK1 131..132)');
  ok(decodeEfuses({ blk0_w2: 0, blk0_w3: 0, blk0_w4: 1 << 5, blk1_w4: 0 }).secureDownload, 'efuse: secure download mode');
  const seq = (n) => new Uint8Array(new Uint32Array([n]).buffer);
  ok(otaCrc(seq(3)) === 0xed4a5011 && otaCrc(seq(4)) === 0x709d68a8, 'otadata: CRC as the bootloader (stock entries)');
  const blank = new Uint8Array(0x2000).fill(0xff);
  ok(otaBoot(blank).slot === 0, 'otadata: blank boots app0');
  const s1 = otaSelect(blank, 1);
  const od = blank.slice(); od.set(s1.sector, s1.address - OTADATA);
  ok(otaBoot(od).slot === 1 && s1.seq === 2, 'otadata: select app1 from blank');
  const s0 = otaSelect(od, 0);
  od.set(s0.sector, s0.address - OTADATA);
  ok(otaBoot(od).slot === 0 && s0.seq === 3 && s0.address !== s1.address, 'otadata: then app0, in the other sector');
  const st = {};
  readStartLine('FWSTATE {"board_ok":true,"why":"","flash_octal":0}', st);
  readStartLine('FWSTATE {"dashboard":"http://192.168.1.50:8080/","paired":0}', st);
  readStartLine('FWMEM {"stage":"boot"}', st);
  readStartLine('garbage {', st);
  ok(st.boardOk === true && st.dashboard === 'http://192.168.1.50:8080/', 'start lines: board check and dashboard');
  readStartLine('FWPANIC {"reason":"x"}', st);
  ok(st.panic && st.panic.startsWith('FWPANIC'), 'start lines: a crash');
}

// ---------------------------------------------------------------- with the real flash
if (!existsSync(BACKUP) || !existsSync(IMAGE)) {
  console.log('SKIP flash tests: need', existsSync(BACKUP) ? IMAGE : BACKUP);
} else {
  const stock = new Uint8Array(readFileSync(BACKUP));
  const base = new Uint8Array(readFileSync(IMAGE));
  ok(stock.length === FLASH_BYTES, 'backup: 8 MB');
  ok(partitionsMatch(parsePartitions(stock.subarray(0x8000, 0x8c00))), 'backup: partition table matches');
  ok(otaBoot(stock.subarray(OTADATA, OTADATA + 0x2000)).slot === 1, 'backup: stock boots app1 (seq 4)');
  ok(appDescription(base).project === 'freebirdwall', 'image: owned project name');
  {   // a wall installed before the rename reports itself as flightwall-owned: still ours
    const legacy = base.slice(0, 0x1000);
    legacy.fill(0, 80, 112);
    legacy.set(new TextEncoder().encode('flightwall-owned'), 80);
    ok(slotKind(appDescription(legacy)) === 'owned', 'image: the name before the rename counts as owned');
  }

  // packaging = package_owned.py, byte for byte
  {
    const key = new Uint8Array(32).map((_, i) => (i * 37 + 11) & 0xff);
    const ours = await packImage(base, key);
    const dir = mkdtempSync(join(tmpdir(), 'fw-pack-'));
    writeFileSync(join(dir, 'base.bin'), base);
    const py = `import sys; sys.path.insert(0, r'${join(REPO, 'runtime', 'firmware', 'tools')}')\n` +
      `import package_owned as p\nc, _ = p.pack(open(r'${join(dir, 'base.bin')}','rb').read(), bytes.fromhex('${hex(key)}'))\n` +
      `open(r'${join(dir, 'py.bin')}','wb').write(c)\n`;
    execFileSync('py', ['-c', py]);
    const theirs = new Uint8Array(readFileSync(join(dir, 'py.bin')));
    ok(hex(ours) === hex(theirs), 'pack: identical to package_owned.py');
    ok(hex(installedKey(ours)) === hex(key), 'pack: the key reads back');
    await throws(() => packImage(ours, key), /already packaged|not unique/, 'pack: refuses a packaged image');
    const broken = base.slice(); broken[2000] ^= 1;
    await throws(() => packImage(broken, key), /checksum|SHA/, 'pack: refuses a damaged build');
  }

  // the board check
  const good = () => new FakeDevice(stock.slice());
  {
    const r = await check(good());
    ok(r.ok && r.mode === 'install' && r.bootSlot === 1 && r.kinds.join() === 'vendor,vendor', 'check: stock board passes, install');
    const fails = async (dev, id, what) => {
      const rr = await check(dev);
      ok(!rr.ok && rr.mode === 'stop' && rr.checks.find((c) => c.id === id && !c.ok), what);
    };
    await fails(new FakeDevice(stock.slice(), { chip: 'ESP32' }), 'chip', 'check: another chip stops');
    await fails(new FakeDevice(stock.slice(), { efuse: { [EFUSE_WORDS.blk0_w4]: 1 << 9 } }), 'octal', 'check: octal flash stops');
    await fails(new FakeDevice(stock.slice(), { efuse: { [EFUSE_WORDS.blk1_w4]: 1 << 3 } }), 'octal', 'check: octal PSRAM stops');
    await fails(new FakeDevice(stock.slice(), { efuse: { [EFUSE_WORDS.blk0_w2]: 1 << 18 } }), 'security', 'check: flash encryption stops');
    await fails(new FakeDevice(stock.slice(), { efuse: { [EFUSE_WORDS.blk0_w3]: 1 << 20 } }), 'security', 'check: secure boot stops');
    await fails(new FakeDevice(stock.slice(0, 4 * 1024 * 1024)), 'flash', 'check: 4 MB flash stops');
    const moved = stock.slice(); moved[0x8000 + 32 * 4 + 4] ^= 0x10;   // spiffs offset
    await fails(new FakeDevice(moved), 'partitions', 'check: another layout stops');
    const other = stock.slice(); other.set(new TextEncoder().encode('someone-else\0\0\0\0\0\0\0\0'), SLOTS[1] + 80);
    await fails(new FakeDevice(other), 'firmware', 'check: unknown firmware stops');
  }

  // install, on a copy of the stock flash
  const dev = good();
  {
    const r = await check(dev);
    const res = await install(dev, r, base);
    ok(res.target === 0, 'install: into the slot not running (app0)');
    ok(otaBoot(dev.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 0, 'install: app0 starts next');
    ok(hex(dev.flash.subarray(SLOTS[1], SLOTS[1] + 0x2c0000)) === hex(stock.subarray(SLOTS[1], SLOTS[1] + 0x2c0000)), 'install: the vendor firmware in app1 is untouched');
    ok(hex(dev.flash.subarray(0, 0xe000)) === hex(stock.subarray(0, 0xe000)), 'install: bootloader, table, Wi-Fi and settings (NVS) untouched');
    ok(hex(dev.flash.subarray(0x590000)) === hex(stock.subarray(0x590000)), 'install: the vendor assets untouched');
    ok(dev.writes.length === 2 && dev.writes[0].addr === SLOTS[0] && dev.writes[1].addr >= OTADATA, 'install: two writes, image then boot choice');
    const installed = await readSlotImage(dev, 0);
    ok(installed && hex(installedKey(installed)) === hex(res.key), 'install: the image on the board carries the new key');
    const again = await check(dev);
    ok(again.ok && again.mode === 'update' && again.bootSlot === 0 && again.kinds.join() === 'owned,vendor', 'install: checked again, it is an update');

    // switch back to the vendor firmware: only the boot choice
    const before = dev.flash.slice();
    const sw = await switchToVendor(dev, again);
    ok(sw.changed && sw.slot === 1 && otaBoot(dev.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 1, 'switch: vendor starts next');
    let diff = 0; for (let i = 0; i < before.length; ++i) if (before[i] !== dev.flash[i]) { if (i < OTADATA || i >= OTADATA + 0x2000) ++diff; }
    ok(diff === 0, 'switch: nothing but otadata changed');

    // an update from the vendor firmware is an install again (new key);
    // from the owned firmware it keeps the key, and the vendor firmware: it
    // replaces the running FreeBirdWall, after pointing the next boot at the
    // vendor firmware, so a stop at any point starts that
    const sel = otaSelect(dev.flash.subarray(OTADATA, OTADATA + 0x2000), 0);
    dev.flash.set(sel.sector, sel.address);
    const up = await check(dev);
    const vendorBefore = hex(dev.flash.subarray(SLOTS[1], SLOTS[1] + 0x2c0000));
    dev.writes = [];
    const res2 = await install(dev, up, base);
    ok(up.mode === 'update' && res2.target === 0 && hex(res2.key) === hex(res.key), 'update: over the running FreeBirdWall, same key');
    ok(hex(dev.flash.subarray(SLOTS[1], SLOTS[1] + 0x2c0000)) === vendorBefore, 'update: the vendor firmware in app1 is untouched');
    ok(dev.writes.length === 3 && dev.writes[0].addr >= OTADATA && dev.writes[0].addr < SLOTS[0] && dev.writes[1].addr === SLOTS[0] &&
       dev.writes[2].addr >= OTADATA && dev.writes[2].addr < SLOTS[0], 'update: boot choice to the vendor firmware, image, boot choice back');
    ok(otaBoot(dev.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 0, 'update: FreeBirdWall starts next');
    const after = await check(dev);
    ok(after.mode === 'update' && after.kinds.join() === 'owned,vendor', 'update: checked again, the vendor firmware is still there');
    const res3 = await install(dev, after, base);
    ok(res3.target === 0 && hex(dev.flash.subarray(SLOTS[1], SLOTS[1] + 0x2c0000)) === vendorBefore, 'update: again, and the vendor firmware stays');
    const sw2 = await switchToVendor(dev, await check(dev));
    ok(sw2.changed && sw2.slot === 1, 'update: the vendor firmware can still be started after two updates');
  }

  // an update cut off while it writes over the running FreeBirdWall: the
  // vendor firmware starts, and the error says so
  {
    const d = good();
    await install(d, await check(d), base);
    const cut = new FakeDevice(d.flash.slice());
    const real = cut.writeFlash.bind(cut);
    cut.writeFlash = async (addr, bytes, p) => { if (addr === SLOTS[0]) { cut.flash.fill(0xff, addr, addr + 0x10000); throw new Error('the cable came out'); } return real(addr, bytes, p); };
    let err = null;
    try { await install(cut, await check(cut), base); } catch (e) { err = e; }
    ok(err && /cable/.test(err.message) && err.starts === 'the vendor firmware', 'update cut off: the error says the vendor firmware starts');
    ok(otaBoot(cut.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 1, 'update cut off: the board starts the vendor firmware');
    const re = await check(cut);
    ok(re.ok && re.mode === 'install' && re.bootSlot === 1, 'update cut off: checked again, it installs from the vendor firmware');
  }

  // an update on a board whose vendor firmware an older installer replaced:
  // the other slot, as before
  {
    const d = good();
    const r1 = await install(d, await check(d), base);
    const old = await packImage(base, r1.key);
    d.flash.set(old, SLOTS[1]);                  /* FreeBirdWall in both slots */
    const r = await check(d);
    ok(r.kinds.join() === 'owned,owned', 'no vendor left: both slots FreeBirdWall');
    const res = await install(d, r, base);
    ok(res.target === 1 && otaBoot(d.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 1, 'no vendor left: the update goes to the slot not running');
  }

  // a write that does not read back: nothing else changes
  {
    const bad = new FakeDevice(stock.slice(), { corruptWrites: true });
    const r = await check(bad);
    await throws(() => install(bad, r, base), /did not read back/, 'install: a bad write stops before the boot choice');
    ok(otaBoot(bad.flash.subarray(OTADATA, OTADATA + 0x2000)).slot === 1, 'install: the board still starts the vendor firmware');
  }

  // backup and restore
  {
    const b = await backup(good());
    ok(b.bytes.length === FLASH_BYTES && b.sha256.length === 64, 'backup: whole flash and its hash');
    await restoreBackup(dev, b.bytes);
    ok(hex(dev.flash) === hex(stock), 'restore: the board is exactly the backup again');
    await throws(() => restoreBackup(dev, b.bytes.subarray(0, 1024)), /exactly 8 MB/, 'restore: refuses a short file');
    const foreign = b.bytes.slice(); foreign.fill(0xff, 0x8000, 0x9000);
    await throws(() => restoreBackup(dev, foreign), /not a FlightWall backup/, 'restore: refuses another board layout');
  }
}

console.log(`${failed ? 'FAIL' : 'PASS'}: ${checks} installer checks${failed ? `, ${failed} failed` : ''}`);
process.exit(failed ? 1 : 0);
