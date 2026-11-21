#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Opens the configurator in headless Chrome or Chromium over the DevTools protocol (no test framework, no
// download) twice: index.html served over http from a local server (every script and the stylesheet SRI-checked)
// and offline.html opened from its file. Checks that each loads with no console error, no CSP violation and no
// failed integrity check, that a shared link restores a selection, that the findings show, that the ZIP builds, and
// that "Open an existing project" reads a kb-image.yml and its lock from a file input and shows the upgrade view.
// Optionally saves screenshots (of the served page).
//
//   node tools/headless_check.mjs [--chrome PATH] [--shots DIR]
//
// Exit status 0 when every check passes, 1 otherwise, 2 when no browser is found (CI then relies on the node
// tests alone).

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, mkdtempSync, rmSync, writeFileSync, mkdirSync, readFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
let page = '';
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.json': 'application/json' };
const server = createServer((req, res) => {
  const path = resolve(root, '.' + decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!path.startsWith(root) || !existsSync(path) || !statSync(path).isFile()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'content-type': types[path.slice(path.lastIndexOf('.'))] || 'application/octet-stream' });
  res.end(readFileSync(path));
});
await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
const passes = [
  { name: 'served index.html', url: `http://127.0.0.1:${server.address().port}/index.html`, shots: true },
  { name: 'offline.html from file', url: pathToFileURL(join(root, 'offline.html')).href, shots: false },
];
const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : null; };

const candidates = [opt('--chrome'), process.env.CHROME,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'].filter(Boolean);
const chrome = candidates.find((p) => existsSync(p));
if (!chrome) { console.log('headless_check: no Chrome or Chromium found; skipped'); process.exit(2); }

// Chrome is started up to twice, each time with a fresh profile and 60 seconds to print its DevTools address: a
// cold runner has been seen to take longer than the 15 seconds this once allowed. The reason of a failed start
// (exit code or the end of Chrome's own output) is in the error, so a flake can be told from a broken install.
const STARTUP_MS = 60000;
const launch = () => {
  const profile = mkdtempSync(join(tmpdir(), 'kb-conf-'));
  const child = spawn(chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--hide-scrollbars',
    '--disable-gpu', '--disable-dev-shm-usage', 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
  const url = new Promise((ok, fail) => {
    let buf = '';
    const timer = setTimeout(() => fail(new Error(`Chrome did not start in ${STARTUP_MS / 1000} s: ${buf.slice(-400)}`)),
      STARTUP_MS);
    child.stderr.on('data', (d) => {
      buf += d;
      const m = /DevTools listening on (ws:\/\/\S+)/.exec(buf);
      if (m) { clearTimeout(timer); ok(m[1]); }
    });
    child.on('exit', (code) => { clearTimeout(timer); fail(new Error(`Chrome exited with ${code}: ${buf.slice(-400)}`)); });
  });
  return { child, profile, url };
};
let started = launch();
let wsUrl;
try {
  wsUrl = await started.url;
} catch (first) {
  console.log(`headless_check: ${first.message}; trying once more`);
  started.child.kill('SIGKILL');
  started = launch();
  wsUrl = await started.url;
}
const { child: proc, profile } = started;

const browser = new WebSocket(wsUrl);
await new Promise((ok) => browser.addEventListener('open', ok, { once: true }));
let seq = 0;
const pending = new Map();
const listeners = [];
browser.addEventListener('message', (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    const { ok, fail } = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) fail(new Error(msg.error.message)); else ok(msg.result);
  } else if (msg.method) {
    for (const l of listeners) l(msg);
  }
});
const send = (method, params = {}, sessionId) => new Promise((ok, fail) => {
  const id = ++seq;
  pending.set(id, { ok, fail });
  browser.send(JSON.stringify({ id, method, params, sessionId }));
});

const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
const s = (m, p) => send(m, p, sessionId);
const problems = [];
listeners.push((msg) => {
  if (msg.sessionId !== sessionId) return;
  if (msg.method === 'Runtime.exceptionThrown') problems.push('exception: ' + msg.params.exceptionDetails.exception?.description);
  if (msg.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(msg.params.type)) {
    problems.push(`console.${msg.params.type}: ` + msg.params.args.map((a) => a.value ?? a.description).join(' '));
  }
  if (msg.method === 'Log.entryAdded' && ['error', 'warning'].includes(msg.params.entry.level)) {
    problems.push(`log ${msg.params.entry.level}: ${msg.params.entry.text}`);
  }
});
await s('Runtime.enable');
await s('Log.enable');
await s('Page.enable');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function open(hash, { width = 1440, height = 1000, scheme = 'light', mobile = false } = {}) {
  await s('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile });
  await s('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: scheme }] });
  await s('Page.navigate', { url: page + (hash || '') });
  await sleep(300);
  await s('Page.reload', { ignoreCache: true });
  await sleep(1200);
}
async function evaluate(expr) {
  const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || 'evaluate failed');
  return r.result.value;
}
let shotsOn = false;
async function shot(name, full) {
  const dir = opt('--shots');
  if (!dir || !shotsOn) return;
  mkdirSync(dir, { recursive: true });
  const params = { format: 'png' };
  if (full) {
    const m = await s('Page.getLayoutMetrics');
    params.clip = { x: 0, y: 0, width: m.cssContentSize.width, height: Math.min(m.cssContentSize.height, 6000), scale: 1 };
    params.captureBeyondViewport = true;
  }
  const { data } = await s('Page.captureScreenshot', params);
  writeFileSync(join(dir, name + '.png'), Buffer.from(data, 'base64'));
}

const checks = [];
let prefix = '';
const check = (name, ok, detail) => { checks.push({ name: prefix + name, ok: !!ok, detail }); };

// The newest kb-core in the catalogue: a release once one exists, else the development revision.
const kbNewest = JSON.parse(readFileSync(join(root, 'data', 'catalog.json'), 'utf8')).kb_core.releases[0];
const kbWarning = kbNewest.kind === 'dev' ? 'CFG-KB-003' : 'CFG-RC-001';
const example = `#v=1&f=esp32&t=esp32s3&b=esp32s3-devkitc&r=freertos&k=${kbNewest.version}&p=&n=weather-node`
  + '&d=bmp581~0.1.0-rc.1~i2c~i2c0,bno08x~0.1.0-rc.1~spi~spi0,pf1550~0.1.0-rc.1~i2c~i2c0,ws2812_rmt~0.1.0-rc.1~pio~pio0'
  + '&m=i2c0~g,pio0~o,spi0~o';
for (const pass of passes) {
page = pass.url;
prefix = pass.name + ': ';
shotsOn = pass.shots;
problems.length = 0;
try {
  await open('');
  check('page loads, catalogue rendered', await evaluate('document.querySelectorAll("#catalogue .card").length') > 20);
  check('default hash written', (await evaluate('location.hash')).startsWith('#v=1'));
  const rcState = await evaluate(`(() => {
    const rcOnly = KB_CATALOG.drivers.find((d) => d.versions.length && d.versions.every((v) => v.kind === 'rc'));
    const before = { toggle: document.getElementById('show-rc').checked, add: rcOnly ? document.getElementById('add-' + rcOnly.name).disabled : true };
    document.getElementById('show-rc').click();
    const after = { add: rcOnly ? document.getElementById('add-' + rcOnly.name).disabled : false, hash: location.hash.includes('c=1') };
    document.getElementById('show-rc').click();
    return { before, after, off: !location.hash.includes('c=1') };
  })()`);
  check('release candidates hidden by default, shown by the switch', !rcState.before.toggle && rcState.before.add
    && !rcState.after.add && rcState.after.hash && rcState.off, rcState);
  await evaluate(`document.getElementById('search').value = 'zz-no-such-chip 9'; document.getElementById('search').dispatchEvent(new Event('input'))`);
  const ask = await evaluate(`(() => { const p = document.getElementById('request-chip'); const a = p.querySelector('a');
    return { shown: !p.hidden, href: a ? a.href : '' }; })()`);
  check('no match offers the request form, part text filtered', ask.shown
    && ask.href === 'https://github.com/kokeb-labs-drivers/driver-requests/issues/new?template=request-driver.yml&part=zz-no-such-chip9', ask);
  await evaluate(`document.getElementById('search').value = ''; document.getElementById('search').dispatchEvent(new Event('input'))`);
  await shot('01-start-light');

  await open(example);
  const state = await evaluate(`({
    selected: document.querySelectorAll('.card[data-selected="true"]').length,
    errors: [...document.querySelectorAll('.finding.error code')].map(e => e.textContent),
    warnings: [...document.querySelectorAll('.finding.warning code')].map(e => e.textContent),
    buses: [...document.querySelectorAll('.bus h3')].map(e => e.id),
    tabs: document.querySelectorAll('[role=tab]').length,
    zip: !document.getElementById('zip').disabled,
  })`);
  check('shared link restores four drivers', state.selected === 4, state);
  check('resolves with no error', state.errors.length === 0, state.errors);
  check('rc, development-revision and untested warnings shown', ['CFG-RC-001', kbWarning, 'CFG-SET-001'].every((c) => state.warnings.includes(c)), state.warnings);
  check('nine file tabs', state.tabs === 9, state.tabs);
  check('shared resolver loaded and agrees', await evaluate('!!KB.engine && KB.shared.run(KB_CATALOG, KB.state.decode(location.hash)).shared.ok'));
  check('lock is the shared resolver output', await evaluate(`(() => { const i = KB.state.decode(location.hash); const r = KB.shared.run(KB_CATALOG, i);
    return KB.generate.generate(KB_CATALOG, i, r).files.find((f) => f.id === 'lock').text === KB.engine.files(r.shared.lock)['kb-image.lock.yml']; })()`));
  check('zip enabled', state.zip);
  const zipBytes = await evaluate(`(() => { const i = KB.state.decode(location.hash); const r = KB.shared.run(KB_CATALOG, i);
    const o = KB.generate.generate(KB_CATALOG, i, r);
    const a = KB.generate.zipProject(fflate, o);
    const b = KB.generate.zipProject(fflate, o);
    return [a.length, a.length === b.length && a.every((x, i) => x === b[i]), Object.keys(fflate.unzipSync(a)).length]; })()`);
  check('zip builds, repeatable, has files', zipBytes[0] > 1000 && zipBytes[1] && zipBytes[2] >= 13, zipBytes);
  await shot('02-selection-light', true);
  await evaluate(`document.getElementById('tab-cdx').click()`);
  await sleep(200);
  await open(example, { scheme: 'dark' });
  await shot('03-selection-dark');
  await evaluate(`document.getElementById('tab-fetch').click(); document.getElementById('step-buses').scrollIntoView()`);
  await sleep(300);
  await shot('04-buses-fetch-dark');

  const conflict = `#v=1&f=esp32&t=esp32c3&b=esp32c3-devkitm&r=freertos&k=${kbNewest.version}&p=&n=conflict`
    + '&d=bno08x~0.1.0-rc.1~i2c~i2c0,pca9685~0.1.0-rc.1~i2c~i2c1,ws2812_rmt~0.1.0-rc.1~pio~pio0,dynamixel~0.1.0-rc.1~uart~uart1,fdo2~0.1.0-rc.1~uart~uart1&m=i2c0~g,i2c1~g,pio0~o,uart1~o';
  await open(conflict);
  const errs = await evaluate(`[...document.querySelectorAll('.finding.error code')].map(e => e.textContent)`);
  check('conflicts explained', ['CFG-THR-002', 'CFG-BUS-003', 'CFG-BUS-004'].every((c) => errs.includes(c)), errs);
  check('zip disabled on error', await evaluate(`document.getElementById('zip').disabled`));
  await evaluate(`document.getElementById('step-buses').scrollIntoView()`);
  await sleep(200);
  await shot('05-conflicts-light');

  await open('#v=1&f=stm32&t=stm32h7&b=nucleo-h743zi&r=threadx&n=planned', {});
  check('planned family and RTOS flagged', (await evaluate(`[...document.querySelectorAll('.finding.error code')].map(e => e.textContent)`)).includes('CFG-MCU-004'));

  await open(example, { width: 390, height: 844, mobile: true });
  check('no horizontal scroll at phone width', await evaluate('document.documentElement.scrollWidth <= innerWidth + 1'),
    await evaluate('document.documentElement.scrollWidth'));
  await shot('06-phone-light');

  await open('', {});
  const opened = await evaluate(`(async () => {
    const cat = KB.shared.kbCatalog(KB_CATALOG);
    const ver = (n) => cat.drivers.find((d) => d.name === n).versions[0].version;
    const img = { schema: 1, image: 'old-project', kb_core: { version: cat.kb_core.versions[0].version },
      mcu: { family: 'esp32', target: 'esp32s3' }, rtos: 'freertos',
      drivers: [{ name: 'bmp581', version: ver('bmp581') }, { name: 'bno08x', version: ver('bno08x') }],
      buses: { i2c0: { model: 'guarded', devices: ['bmp581'] }, spi1: { model: 'owner', devices: ['bno08x'] } } };
    const r = KB.engine.resolve(cat, img);
    const dt = new DataTransfer();
    dt.items.add(new File([KB.engine.imageText(img)], 'kb-image.yml', { type: 'text/yaml' }));
    dt.items.add(new File([KB.engine.files(r.lock)['kb-image.lock.yml']], 'kb-image.lock.yml', { type: 'text/yaml' }));
    const input = document.getElementById('open-files');
    input.files = dt.files;
    input.dispatchEvent(new Event('change'));
    await new Promise((ok) => setTimeout(ok, 400));
    const plan = KB.engine.plan(cat, img, { lock: r.lock });
    return {
      visible: !document.getElementById('upgrade').hidden,
      rows: document.querySelectorAll('#upgrade-table tbody tr').length,
      selected: document.querySelectorAll('.card[data-selected="true"]').length,
      status: document.getElementById('open-status').textContent,
      summary: document.getElementById('upgrade-summary').textContent,
      clean: !/null|undefined/.test(document.getElementById('upgrade').textContent),
      siteErrors: document.querySelectorAll('.finding.error').length,
      buttons: !document.getElementById('upgrade-report').disabled && !document.getElementById('upgrade-apply').disabled,
      hashFree: !location.hash.includes('old-project'),
      plan: plan.ok && plan.rows.length === 4,
    };
  })()`);
  check('open project: files read, selection restored', opened.visible && opened.selected === 2 && /Opened/.test(opened.status), opened);
  check('open project: upgrade view lists kb-core, the ESP32 port and both drivers', opened.rows === 4 && opened.plan && opened.buttons && opened.clean, opened);
  check('open project: the opened project resolves on the page', opened.siteErrors === 0, opened);
  check('open project: up to date against this catalogue', /Up to date|can move/.test(opened.summary), opened.summary);
  check('open project: the project name stays out of the address bar', opened.hashFree, opened);
  const refused = await evaluate(`(async () => {
    const sel = document.getElementById('to-kb-core');
    sel.append(new Option('9.9.9', '9.9.9'));
    sel.value = '9.9.9';
    sel.dispatchEvent(new Event('change'));
    await new Promise((ok) => setTimeout(ok, 100));
    const during = { rows: document.querySelectorAll('#upgrade-table tbody tr').length,
      error: [...document.querySelectorAll('#upgrade-findings .finding.error code')].map((e) => e.textContent),
      clear: !document.getElementById('upgrade-clear').hidden };
    document.getElementById('upgrade-clear').click();
    await new Promise((ok) => setTimeout(ok, 100));
    return { during, after: { errors: document.querySelectorAll('#upgrade-findings .finding.error').length,
      clear: !document.getElementById('upgrade-clear').hidden } };
  })()`);
  check('open project: a refused version keeps the table and can be cleared', refused.during.rows === 4
    && refused.during.error.includes('KB-UPG-004') && refused.during.clear && refused.after.errors === 0 && !refused.after.clear, refused);
  await evaluate(`document.getElementById('allow-rc').click()`);
  await sleep(200);
  check('open project: release-candidate toggle keeps the view', await evaluate(`document.querySelectorAll('#upgrade-table tbody tr').length`) === 4);
  await evaluate(`document.getElementById('step-open').scrollIntoView()`);
  await sleep(200);
  await shot('08-open-project-upgrades');
  await evaluate(`document.getElementById('reset').click()`);
  await sleep(200);
  check('start over closes the opened project', await evaluate(`document.getElementById('upgrade').hidden`));

  await evaluate(`document.getElementById('search').value = 'pressure'; document.getElementById('search').dispatchEvent(new Event('input'))`);
  check('search filters the catalogue', await evaluate(`document.querySelectorAll('#catalogue .card:not([hidden])').length`) < 5);
  await evaluate(`document.querySelector('#catalogue .card:not([hidden]) .thumb').click()`);
  await sleep(400);
  check('figure dialog opens', await evaluate(`document.getElementById('figure-dialog').open`));
  await shot('07-figure-dialog');
} catch (e) {
  check('run', false, String(e));
}
check('no console error, CSP or SRI failure', problems.length === 0, problems);
}
server.close();

browser.close();
const exited = new Promise((r) => proc.once('exit', r));
proc.kill();
await exited;
// Chrome's helper processes can still write to the profile for a moment after the browser exits; the profile is a
// temporary folder, so failing to remove it is reported, never a failed check.
try {
  rmSync(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 500 });
} catch (e) {
  console.warn(`headless_check: could not remove the temporary profile ${profile}: ${e.code || e}`);
}
let failed = 0;
for (const c of checks) {
  if (!c.ok) failed++;
  console.log(`${c.ok ? 'ok  ' : 'FAIL'} ${c.name}${c.ok ? '' : ' ' + JSON.stringify(c.detail)}`);
}
console.log(`headless_check: ${checks.length - failed} of ${checks.length} passed`);
process.exit(failed ? 1 : 0);
