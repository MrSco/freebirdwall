// Runs a preview scenario through main/preview.wasm, as tests/preview_host.c
// does through the desktop build (tests/test_preview.py compares them).
//   node preview_run.mjs <preview.wasm> <scenario.json> <outdir>
// The loader is the one the dashboard uses (main/dashboard.html, "preview").
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [wasmPath, scenarioPath, outDir] = process.argv.slice(2);
const wasi = {
  fd_prestat_get: () => 8,                 // EBADF: no preopened directories
  fd_prestat_dir_name: () => 8,
  proc_exit: (code) => { throw new Error('preview exited ' + code); },
  random_get: (ptr, len) => { new Uint8Array(mod.memory.buffer, ptr, len).fill(7); return 0; },
};
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), { wasi_snapshot_preview1: wasi });
const mod = instance.exports;
mod._initialize();
const enc = new TextEncoder();
const put = (bytes) => { const p = mod.pv_alloc(bytes.length + 1); const m = new Uint8Array(mod.memory.buffer, p, bytes.length + 1); m.set(bytes); m[bytes.length] = 0; return p; };
const str = (s) => put(enc.encode(s));
const cstr = (p) => { if (!p) return null; const m = new Uint8Array(mod.memory.buffer); let e = p; while (m[e]) e++; return new TextDecoder().decode(m.subarray(p, e)); };
const call = (fn, ...strs) => { const ps = strs.map(str); fn(...ps); ps.forEach(mod.pv_free); };

let changed = 0;
for (const c of JSON.parse(readFileSync(scenarioPath, 'utf8'))) {
  const [k, v] = Object.entries(c)[0];
  if (k === 'settings') call(mod.pv_settings, JSON.stringify(v));
  else if (k === 'flights') call(mod.pv_flights, JSON.stringify(v));
  else if (k === 'aliases') call(mod.pv_aliases, JSON.stringify(v));
  else if (k === 'mode') mod.pv_mode(v);
  else if (k === 'logo') {
    const png = v[2] ? readFileSync(v[2]) : new Uint8Array(0);
    const al = str(v[0]), ac = str(v[1]), p = png.length ? put(png) : 0;
    mod.pv_logo(al, ac, p, png.length);
    [al, ac, p].forEach((x) => x && mod.pv_free(x));
  } else if (k === 'step') changed += mod.pv_step(v[0] >>> 0, v[1]);
  else if (k === 'dump') {
    writeFileSync(join(outDir, v + '.rgba'), new Uint8Array(mod.memory.buffer, mod.pv_frame(), 128 * 64 * 4));
    const wanted = [];
    for (let i = 0, w; (w = cstr(mod.pv_logo_wanted(i))); i++) wanted.push(w);
    console.log([v, changed, ...wanted].join(' '));
    changed = 0;
  }
}
