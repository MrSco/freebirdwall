/* Browser regression checks. Requires Playwright and Chromium. Download the
 * pinned Leaflet assets to runtime/build/deps/leaflet/ before running; see
 * SETTINGS.md. All map tiles, search results and wall APIs are local fixtures. */
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFile, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
const { chromium } = createRequire(import.meta.url)('playwright');
const dashboard = await readFile(new URL('../main/dashboard.html', import.meta.url), 'utf8');
const leaflet = new URL('../../build/deps/leaflet/', import.meta.url);
const js = await readFile(new URL('leaflet.js', leaflet));
const css = await readFile(new URL('leaflet.css', leaflet));
const artifacts = new URL('../../build/dashboard-tests/', import.meta.url);
await mkdir(artifacts, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium', args: ['--no-sandbox'] });
const context = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, reducedMotion: 'reduce', colorScheme: 'dark' });
const errors = [], writes = [], external = [];
let revision = 1, offline = false;
let settings = {
 display_config: { brightness_percent: 30, sleep_time: { start: '22:00', end: '07:00', sleep_brightness_percent: 3 } },
 request_config: { radius_requests: [{ name: 'Boca', latitude: 26.37720236, longitude: -80.11265262, radius_km: 3.2 }, { name: 'FLL', latitude: 26.07, longitude: -80.14, radius_km: 25 }] },
 owned_config: { flight_source: 'community' }
};
const snapshot = () => ({ revision, settings, aliases: { revision: 0, aliases: [] }, feed: 3 });
await context.addInitScript(() => { localStorage.setItem('fwkey', 'test-wall-key'); localStorage.setItem('fwtab', 'display'); });
await context.route('**/*', async route => {
 const request = route.request(), url = new URL(request.url());
 if (url.origin !== 'http://wall.test') {
  external.push(request);
  assert.equal(request.headers().authorization, undefined, 'wall key never sent to external services');
  if (url.hostname === 'unpkg.com') {
   if (offline) return route.abort();
   return route.fulfill({ body: url.pathname.endsWith('.css') ? css : js, contentType: url.pathname.endsWith('.css') ? 'text/css' : 'application/javascript', headers: { 'Access-Control-Allow-Origin': '*' } });
  }
  if (url.hostname === 'nominatim.openstreetmap.org') return route.fulfill({ json: [{ lat: '26.0726', lon: '-80.1527', display_name: 'Fort Lauderdale-Hollywood International Airport' }] });
  // A simple local tile keeps tests independent of the public tile server.
  if (url.hostname === 'tile.openstreetmap.org') return route.fulfill({ contentType: 'image/svg+xml', body: '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256"><rect width="256" height="256" fill="#dce9dc"/><path d="M0 100h256M90 0v256" stroke="#fff" stroke-width="12"/></svg>' });
  return route.abort();
 }
 if (url.pathname === '/') return route.fulfill({ body: dashboard, contentType: 'text/html' });
 if (url.pathname === '/api/settings') {
  if (request.method() === 'PUT') {
   const edit = request.postDataJSON(); writes.push(edit);
   for (const [section, values] of Object.entries(edit)) Object.assign(settings[section], values);
   revision++;
  }
  return route.fulfill({ json: snapshot() });
 }
 if (url.pathname === '/api/sync') return route.fulfill({ json: { revision: 0, enabled: false, legs: [] } });
 if (url.pathname.startsWith('/api/')) return route.fulfill({ json: {} });
 return route.fulfill({ status: 204 });
});
const page = await context.newPage();
page.on('pageerror', error => errors.push(error.stack));
const tab = name => page.getByRole('tab', { name, exact: true }).click();
const saveChanges = async () => { await page.locator('#bar-save').click(); await page.waitForFunction(() => !document.querySelector('#bar').classList.contains('show')); };
const canvas = () => page.locator('#wall-preview').evaluate(c => c.toDataURL());
const pixel = (x,y) => page.locator('#wall-preview').evaluate((c,p) => Array.from(c.getContext('2d').getImageData(...p,1,1).data), [x,y]);
const setRange = (selector,value) => page.locator(selector).evaluate((el,v) => {el.value=v;el.dispatchEvent(new Event('input',{bubbles:true}));}, value);
try {
 await page.goto('http://wall.test/');
 await page.locator('#wall-preview').waitFor();
 assert.equal(external.length, 0, 'no external requests on Display');
 for (const width of [320, 360, 390, 430, 768, 1024]) {
  await page.setViewportSize({ width, height: 900 });
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `page fits at ${width}px`);
  for (const input of await page.locator('input[type=time]').all()) assert(await input.evaluate(el => { const r=el.getBoundingClientRect(), p=el.closest('.card').getBoundingClientRect(); return r.right<=p.right-10 && r.left>=p.left; }), `time input stays in card at ${width}px`);
 }
 await page.setViewportSize({ width: 390, height: 844 });
 const initial = await canvas();
 await page.locator('[data-set="display_config.layout_template"][data-val=\'"NO_LOGO"\']').click();
 assert.notEqual(await canvas(), initial, 'layout updates preview');
 const noLogo = await canvas();
 await page.locator('[data-set="display_config.title_text"][data-val=\'"registration"\']').click();
 assert.notEqual(await canvas(), noLogo, 'title updates preview');
 const before = await pixel(127,63);
 assert(before[0]>0, 'heartbeat defaults on in preview');
 await page.locator('label[for="f_owned_config_heartbeat_enabled"]').click();
 assert.deepEqual(await pixel(127,63), [0,0,0,255], 'heartbeat disabled in preview');
 await page.locator('label[for="f_display_config_border_enable"]').click();
 assert((await pixel(0,0))[1]>0, 'border preview enabled');
 await page.getByRole('button', { name: 'Asleep', exact: true }).click();
 await setRange('[data-bind="display_config.sleep_time.sleep_brightness_percent"]', 0);
 assert.match(await page.locator('#preview-note').textContent(), /dark/);
 await page.getByRole('button', { name: 'Flight', exact: true }).click();
 await page.screenshot({ path: fileURLToPath(new URL('display-mobile.png', artifacts)), fullPage: true });
 await saveChanges();
 assert.equal(writes.at(-1).owned_config.heartbeat_enabled, false, 'heartbeat in saved patch');
 await tab('Flights');
 assert.equal(await page.locator('.area-editor').count(), 0, 'existing areas collapsed');
 assert.equal(external.length, 0, 'collapsed areas do not load map');
 await page.locator('[data-open-zone="0"]').click();
 await page.locator('.leaflet-container').waitFor();
 assert.equal(await page.locator('#bar').evaluate(el=>el.classList.contains('show')), false, 'opening map does not round saved coordinates or dirty settings');
 await page.locator('#map-query').fill('FLL airport');
 await page.locator('#map-search button').click();
 await page.getByRole('button',{name:'Fort Lauderdale-Hollywood International Airport'}).click();
 assert.match(await page.locator('#map-coordinates').textContent(), /26.0726, -80.1527/);
 await page.locator('.area-pin').scrollIntoViewIfNeeded();
 const pin = await page.locator('.area-pin').boundingBox();
 const beforeDrag = await page.locator('[data-loc="0"]').inputValue();
 await page.mouse.move(pin.x+22,pin.y+22);await page.mouse.down();await page.mouse.move(pin.x+48,pin.y+34,{steps:8});await page.mouse.up();
 assert.notEqual(await page.locator('[data-loc="0"]').inputValue(),beforeDrag,'dragging pin changes centre');
 await setRange('[data-bind="request_config.radius_requests.0.radius_km"]', 40);
 assert.match(await page.locator('[data-zone-summary="0"]').textContent(), /40 km/);
 await page.locator('#area-map').click({ position: { x: 150, y: 100 } });
 assert.notEqual(await page.locator('[data-loc="0"]').inputValue(), '26.0726, -80.1527', 'map tap changes centre');
 await saveChanges();
 assert.equal(writes.at(-1).request_config.radius_requests[0].radius_km, 40);
 assert.equal(writes.at(-1).request_config.radius_requests[1].name, 'FLL', 'other area preserved');
 await page.locator('[data-open-zone="1"]').click();
 assert.equal(await page.locator('.area-editor').count(), 1, 'only one editor expanded');
 await page.locator('.leaflet-container').waitFor();
 await page.locator('[data-open-zone="1"]').click();
 assert.equal(await page.locator('.area-editor').count(), 0, 'area collapses');
 await page.locator('#add-zone').click();
 await page.locator('.leaflet-container').waitFor();
 await page.locator('#bar-save').click();
 assert.match(await page.locator('#toast').textContent(), /Choose a centre/);
 await page.locator('#area-map').click({ position: { x: 160, y: 110 } });
 await page.locator('[data-bind="request_config.radius_requests.2.name"]').fill('New airport');
 await page.locator('[data-bind="request_config.radius_requests.2.name"]').blur();
 assert.equal(await page.locator('[data-open-zone="2"] strong').textContent(), 'New airport');
 await saveChanges();
 assert.equal(settings.request_config.radius_requests.length, 3);
 await page.screenshot({ path: fileURLToPath(new URL('map-mobile.png', artifacts)), fullPage: true });
 await tab('Display');
 assert.equal(await page.locator('[data-bind="owned_config.heartbeat_enabled"]').isChecked(), false);
 await page.reload();
 await page.locator('#wall-preview').waitFor();
 assert.equal(await page.locator('[data-bind="owned_config.heartbeat_enabled"]').isChecked(), false, 'heartbeat retained after reload');
 // Offline map load is recoverable, with manual entry still available.
 await page.evaluate(() => { delete window.L; }); offline = true;
 await tab('Flights'); await page.locator('[data-open-zone="0"]').click();
 await page.getByText('Map unavailable.', { exact: false }).waitFor();
 await page.getByText('Enter coordinates manually', { exact: true }).click();
 await page.locator('[data-loc="0"]').fill('25.1, -81.2'); await page.locator('[data-loc="0"]').blur();
 assert.match(await page.locator('#map-coordinates').textContent(), /25.1000, -81.2000/);
 offline = false; await page.locator('#map-retry').click(); await page.locator('.leaflet-container').waitFor();
 assert.equal(errors.length, 0, errors.join('\n'));
 await page.screenshot({ path: fileURLToPath(new URL('map-mobile.png', artifacts)), fullPage: true });
 console.log('PASS: dashboard preview, heartbeat save/reload, 6 responsive widths, map tap/search/save, compact areas, offline fallback and retry');
} finally { await browser.close(); }
