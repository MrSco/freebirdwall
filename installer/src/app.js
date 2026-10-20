// The installer page: esptool-js on the browser's Web Serial, driving the
// steps in flow.js. Built into one file by tools/build_installer.py, which
// also embeds esptool-js (as #esptool-src) and the firmware (#firmware).
import { FLASH_BYTES, appDescription, hex, sha256 } from './rules.js';
import { backup, check, install, readStartLine, restoreBackup, switchToVendor } from './flow.js';
import { demoDevice } from './demo.js';

const $ = (s) => document.querySelector(s);
const state = { loader: null, transport: null, dev: null, report: null, backupOk: false, port: null };
const firmware = (() => {
  const el = $('#firmware');
  const bin = atob(el.textContent.trim());
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; ++i) bytes[i] = bin.charCodeAt(i);
  return bytes;
})();
const fwDesc = appDescription(firmware);
const demo = location.hash === '#demo';   // a pretend wall (demo.js): try the page without one

function show(id, on = true) { $(id).hidden = !on; }
function say(id, html, kind = '') { const el = $(id); el.innerHTML = html; el.className = 'msg ' + kind; show(id); }
function progress(id, done, total, label) {
  const el = $(id);
  el.hidden = false;
  el.querySelector('.bar > i').style.width = `${total ? Math.min(100, (100 * done) / total) : 0}%`;
  el.querySelector('.label').textContent = label;
}
const mb = (n) => (n / 1048576).toFixed(1) + ' MB';
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function step(n) { document.querySelectorAll('.step').forEach((s) => s.classList.toggle('current', +s.dataset.step === n)); }

async function esptool() {
  const src = $('#esptool-src').textContent;
  return import(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
}

// ---------------------------------------------------------------- 1. connect
$('#connect').onclick = async () => {
  if (demo) {
    $('#connect').disabled = true;
    state.dev = demoDevice();
    say('#connect-msg', 'Connected: ESP32-S3, 8MB flash. <i>(Demo: a pretend wall, nothing is connected.)</i>', 'good');
    await runCheck();
    return;
  }
  if (!('serial' in navigator)) { say('#connect-msg', 'This browser cannot reach USB devices. Open this file in <b>Chrome</b> or <b>Edge</b> on a computer.', 'bad'); return; }
  try {
    $('#connect').disabled = true;
    say('#connect-msg', 'Choose the FlightWall in the window that opens: <i>USB Serial Device (COM…)</i> on Windows, or <i>USB JTAG/serial debug unit</i>. The panel goes dark while the installer works.');
    const { ESPLoader, Transport } = await esptool();
    state.port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    state.transport = new Transport(state.port, false);
    state.loader = new ESPLoader({ transport: state.transport, baudrate: 921600, terminal: { clean() {}, writeLine() {}, write() {} } });
    say('#connect-msg', 'Talking to the board…');
    await state.loader.main();
    const size = await state.loader.detectFlashSize();
    const flashBytes = (parseInt(size, 10) || 0) * 1048576;
    state.dev = {
      chip: state.loader.chip.CHIP_NAME,
      flashBytes,
      readReg: (a) => state.loader.readReg(a),
      readFlash: (a, n, cb) => state.loader.readFlash(a, n, cb ? (_p, got, total) => cb(got, total) : null),
      writeFlash: (a, data, cb) => state.loader.writeFlash({
        fileArray: [{ data, address: a }], flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
        eraseAll: false, compress: true, reportProgress: cb ? (_i, w, t) => cb(w, t) : undefined,
      }),
    };
    say('#connect-msg', `Connected: ${esc(state.dev.chip)}, ${esc(size)} flash.`, 'good');
    await runCheck();
  } catch (e) {
    $('#connect').disabled = false;
    const why = String(e.message || e).replace(/\.+$/, '');
    say('#connect-msg', /No port selected/.test(why)
      ? 'No device was chosen. Click <b>Connect</b> and pick the wall in the list. If it is not listed, check the cable is a <b>data</b> cable.'
      : `Could not connect: ${esc(why)}.<br>Check the cable is a <b>data</b> cable, then try again. If it keeps failing, hold the BOOT button (S1) on the board while plugging in.`, 'bad');
  }
};

// ---------------------------------------------------------------- 2. check
async function runCheck() {
  step(2);
  show('#check');
  say('#check-msg', 'Checking the board… nothing is written in this step.');
  const r = state.report = await check(state.dev);
  $('#checks').innerHTML = r.checks.map((c) =>
    `<li class="${c.ok ? 'ok' : 'no'}"><span>${c.ok ? '✓' : '✗'}</span><b>${esc(c.label)}</b><small>${esc(c.detail)}</small></li>`).join('');
  if (!r.ok) {
    say('#check-msg', '<b>This board is not one FreeBirdWall was made for, so the installer stops here.</b> Nothing was changed. Unplug it; it keeps working as before.', 'bad');
    return;
  }
  const other = r.kinds[1 - r.bootSlot];
  if (r.mode === 'install') {
    say('#check-msg', 'This is a FlightWall mini running the vendor’s firmware. FreeBirdWall can be installed. The vendor firmware stays on the board and can be started again from this page.', 'good');
  } else {
    say('#check-msg', `This wall already runs FreeBirdWall (${esc(r.descs[r.bootSlot].version)}). Installing updates it to ${esc(fwDesc.version)} and keeps your sign-ins.` +
      (other === 'vendor' ? ' The vendor firmware kept on the board is replaced by the update; your backup can bring it back.' : ''), 'good');
  }
  step(3);
  show('#backup');
  show('#restore-tools');
  $('#vendor').disabled = !r.kinds.includes('vendor') || r.kinds[r.bootSlot] === 'vendor';
}

// ---------------------------------------------------------------- 3. backup
$('#save-backup').onclick = async () => {
  $('#save-backup').disabled = true;
  $('#skip-backup').disabled = true;
  try {
    const name = `flightwall-backup-${new Date().toISOString().slice(0, 10)}.bin`;
    let handle = null;
    if (demo) {
      const b = await backup(state.dev, (done, total) => progress('#backup-progress', done, total, `Reading ${mb(done)} of ${mb(total)}`));
      say('#backup-msg', `Backup saved and checked: <b>${name}</b> (8 MB). <i>(Demo: read, not saved.)</i><br><small>SHA-256 ${b.sha256}</small>`, 'good');
      state.backupOk = true;
      step(4);
      show('#install');
      return;
    }
    if (window.showSaveFilePicker) {
      handle = await window.showSaveFilePicker({ suggestedName: name, types: [{ description: 'FlightWall backup', accept: { 'application/octet-stream': ['.bin'] } }] });
    }
    const b = await backup(state.dev, (done, total) => progress('#backup-progress', done, total, `Reading ${mb(done)} of ${mb(total)}`));
    if (handle) {
      const w = await handle.createWritable();
      await w.write(b.bytes);
      await w.close();
      const saved = new Uint8Array(await (await handle.getFile()).arrayBuffer());
      if (saved.length !== FLASH_BYTES || hex(await sha256(saved)) !== b.sha256) throw new Error('the saved file did not read back the same');
      say('#backup-msg', `Backup saved and checked: <b>${esc(handle.name)}</b> (8 MB). Keep it somewhere safe; it is this wall’s own copy, with its Wi-Fi password and account key, so do not share it.<br><small>SHA-256 ${b.sha256}</small>`, 'good');
    } else {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([b.bytes]));
      a.download = name;
      a.click();
      say('#backup-msg', `Backup downloaded as <b>${name}</b>. Make sure it finished saving (8 MB) before you go on. It holds this wall’s Wi-Fi password and account key: do not share it.<br><small>SHA-256 ${b.sha256}</small>`, 'good');
    }
    state.backupOk = true;
    step(4);
    show('#install');
  } catch (e) {
    $('#save-backup').disabled = false;
    $('#skip-backup').disabled = false;
    say('#backup-msg', `The backup did not finish: ${esc(e.message || e)}. Nothing was changed on the board. Try again.`, 'bad');
  }
};

// Skipping is allowed, but asked twice: without a backup nothing can put the
// wall back exactly as it is now.
$('#skip-backup').onclick = () => {
  const update = state.report && state.report.mode === 'update';
  if (!confirm((update ? 'If you kept the backup from your first install, you can skip this one.\n\n' : '') +
      'Without a backup, the wall cannot be put back exactly as it is now if something goes wrong. Skip the backup?')) return;
  if (!confirm('Are you sure? Install without a backup?')) return;
  state.backupOk = true;
  $('#save-backup').disabled = true;
  $('#skip-backup').disabled = true;
  say('#backup-msg', 'No backup saved this time.' + (update ? ' Keep the one from your first install.' : ''), '');
  step(4);
  show('#install');
};

// ---------------------------------------------------------------- 4. install
$('#do-install').onclick = async () => {
  if (!state.backupOk) return;
  $('#do-install').disabled = true;
  say('#install-msg', 'Installing… keep the cable plugged in.');
  try {
    const res = await install(state.dev, state.report, firmware, (what, done, total) =>
      progress('#install-progress', done, total, what === 'write' ? `Writing ${mb(done)} of ${mb(total)}` : what === 'verify' ? `Checking ${mb(done)} of ${mb(total)}` : 'Reading the key on the board'));
    state.key = res.key;
    $('#vendor').disabled = false;                   /* the vendor firmware is in the other slot now */
    say('#install-msg', `Installed ${esc(res.desc.version)} and checked it. Starting the wall…`, 'good');
    step(5);
    show('#start');
    await watchStart();
  } catch (e) {
    $('#do-install').disabled = false;
    say('#install-msg', `Not installed: ${esc(e.message || e)}. The wall still starts its old firmware.`, 'bad');
  }
};

// ---------------------------------------------------------------- 5. start
// Restarts the wall into its firmware. On the ESP32-S3's USB-Serial/JTAG
// port a reset happens when RTS goes high with DTR low (DTR high would hold
// the boot pin low: flashing mode again). DTR is sent again after each RTS
// change because Windows' usbser.sys passes a line change on only with it
// (esptool.py does the same). Measured on the wall: this gives
// rst:0x15 (USB_UART_CHIP_RESET), boot:0x8 (SPI_FAST_FLASH_BOOT), and the
// port stays open, so the start can be watched. esptool-js 0.6.1's own hard
// reset only sets RTS off, which reset nothing: the wall stayed in the
// flasher with a frozen panel.
async function restart() {
  const pause = (ms) => new Promise((r) => setTimeout(r, ms));
  const t = state.transport;
  await t.setDTR(false);
  await t.setRTS(true);
  await t.setDTR(false);
  await pause(200);
  await t.setRTS(false);
  await t.setDTR(false);
  await pause(100);
}

async function watchStart() {
  const seen = {};
  const decoder = new TextDecoder();
  let buffer = '', closed = false;
  const finish = () => { closed = true; };
  say('#start-msg', 'The wall is starting. Its panel lights up, joins your Wi-Fi and shows a QR code with its address…');
  if (demo) {
    for (const line of state.dev.startLines) { await new Promise((r) => setTimeout(r, 900)); readStartLine(line, seen); }
  }
  const render = () => {
    const rows = [];
    if (seen.boardOk === true) rows.push('<li class="ok"><span>✓</span><b>The firmware checked the board and started the panel</b></li>');
    if (seen.boardOk === false) rows.push(`<li class="no"><span>✗</span><b>The firmware refused this board</b><small>${esc(seen.boardWhy)}</small></li>`);
    if (seen.dashboard) rows.push(`<li class="ok"><span>✓</span><b>On your Wi-Fi</b><small>Dashboard: <a href="${esc(seen.dashboard)}" target="_blank">${esc(seen.dashboard)}</a></small></li>`);
    if (seen.panic) rows.push('<li class="no"><span>✗</span><b>The firmware crashed while starting</b></li>');
    $('#start-checks').innerHTML = rows.join('');
    if (seen.dashboard) done();
  };
  const done = () => {
    if (state.finished) return;
    state.finished = true;
    finish();
    const ip = new URL(seen.dashboard).hostname;
    say('#start-msg', `<b>Done.</b> Open <a href="${esc(seen.dashboard)}" target="_blank">${esc(seen.dashboard)}</a> on a phone or computer on the same Wi-Fi (or scan the QR code on the wall), then press <i>Show a code on the wall</i> to sign in. You can unplug the cable now.`, 'good');
    const keyFile = JSON.stringify({ ip, token: hex(state.key) }, null, 1);
    const a = $('#key-file');
    a.href = URL.createObjectURL(new Blob([keyFile], { type: 'application/json' }));
    a.download = 'flightwall-device.json';
    show('#key-box');
  };
  if (demo) { render(); return; }
  // The flasher's read loop holds the port's reader; cancelling it ends the
  // loop without touching the control lines (a close or open can reset the
  // wall). Then listen first and reset second, so the first lines are seen.
  const port = state.transport.device;
  if (port.readable && port.readable.locked) { try { await state.transport.reader.cancel(); } catch { /* already ending */ } }
  for (let i = 0; i < 40 && port.readable && port.readable.locked; ++i) await new Promise((r) => setTimeout(r, 50));
  const read = state.transport.rawRead((data) => {
    buffer += decoder.decode(data, { stream: true });
    let nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
      readStartLine(buffer.slice(0, nl), seen);
      buffer = buffer.slice(nl + 1);
      render();
    }
  }, () => closed).catch(() => {});
  await restart();
  setTimeout(() => {
    if (state.finished) return;
    finish();
    say('#start-msg', seen.boardOk === false || seen.panic
      ? 'The new firmware did not start properly. Use <b>Start the vendor firmware again</b> below, or restore your backup.'
      : 'The wall did not report its address over USB. Look at the wall: after it joins Wi-Fi it shows a QR code with its address for a minute. If it does not, unplug it, plug it back in, and look again.', seen.boardOk === false || seen.panic ? 'bad' : '');
  }, 90000);
  await read;
}

// ---------------------------------------------------------------- tools
$('#vendor').onclick = async () => {
  if (!confirm('Start the vendor firmware the next time the wall starts? Your FreeBirdWall settings stay on the board.')) return;
  try {
    state.report = await check(state.dev);           /* the board as it is now, not as it was */
    const r = await switchToVendor(state.dev, state.report);
    if (r.changed && !demo) await restart();
    say('#tools-msg', r.changed ? 'Done: the wall is starting the vendor firmware.' : 'The vendor firmware already starts.', 'good');
  } catch (e) { say('#tools-msg', esc(e.message || e), 'bad'); }
};
$('#restore-file').onchange = async (ev) => {
  const file = ev.target.files[0];
  if (!file) return;
  if (!confirm(`Write ${file.name} back over the whole wall? Everything on it becomes exactly as it was in that backup.`)) return;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    await restoreBackup(state.dev, bytes, (what, done, total) => progress('#tools-progress', done, total, `${what === 'write' ? 'Writing' : 'Checking'} ${mb(done)} of ${mb(total)}`));
    if (!demo) await restart();
    say('#tools-msg', 'Backup restored and checked. The wall is starting again.', 'good');
  } catch (e) { say('#tools-msg', `Not restored: ${esc(e.message || e)}`, 'bad'); }
};

$('#fw-version').textContent = fwDesc ? `${fwDesc.version} (${fwDesc.date})` : 'unknown';
if (demo) say('#connect-msg', 'Demo: this page is using a pretend wall. Nothing is connected or changed.');
else if (!('serial' in navigator)) say('#connect-msg', 'This browser cannot reach USB devices. Open this file in <b>Chrome</b> or <b>Edge</b> on a computer.', 'bad');
