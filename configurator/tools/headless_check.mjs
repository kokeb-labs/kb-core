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
// A JSON string literal that is safe inside generated page code (also escapes the characters that end a script).
const UNSAFE = { '<': '\\u003C', '>': '\\u003E', '/': '\\u002F', '\u2028': '\\u2028', '\u2029': '\\u2029' };
const literal = (v) => JSON.stringify(v).replace(/[<>/\u2028\u2029]/g, (c) => UNSAFE[c]);
// A real mouse click at the centre of an element (so the page sees what a person's click does); each one is counted.
let clicks = 0;
async function click(selector) {
  const box = await evaluate(`(() => { const el = document.querySelector(${literal(selector)}); if (!el) return null;
    el.scrollIntoView({ block: 'center' }); const r = el.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2, disabled: !!el.disabled }; })()`);
  if (!box) throw new Error(`no element ${selector}`);
  if (box.disabled) throw new Error(`${selector} is disabled`);
  await s('Input.dispatchMouseEvent', { type: 'mousePressed', x: box.x, y: box.y, button: 'left', clickCount: 1 });
  await s('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box.x, y: box.y, button: 'left', clickCount: 1 });
  clicks++;
  await sleep(120);
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
const catalogData = JSON.parse(readFileSync(join(root, 'data', 'catalog.json'), 'utf8'));
const kbNewest = catalogData.kb_core.releases[0];
const driverCount = catalogData.drivers.length;  // the checks follow the catalogue, not a fixed number
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
  check('page has a description and one h1', await evaluate('!!document.querySelector("meta[name=description]").content && document.querySelectorAll("h1").length === 1'));
  check('advanced is one closed disclosure holding versions, open project, buses, explorer and previews', await evaluate(`(() => {
    const a = document.getElementById('advanced'); if (!a || a.open || a.tagName !== 'DETAILS') return false;
    return ['step-open', 'step-versions', 'step-buses', 'explore', 'tabs'].every((id) => a.contains(document.getElementById(id)))
      && !document.getElementById('board') && !document.getElementById('pins') && [...document.querySelectorAll('details')].filter((d) => !d.closest('#advanced') && !d.closest('#supply')).map((d) => d.id).join() === 'tag-details';
  })()`));
  check('default hash written', (await evaluate('location.hash')).startsWith('#v=1'));
  // Release candidates are hidden by default once kb-core has a final release; before that the page starts on the
  // newest release candidate with the switch on (it would otherwise open with nothing to build on).
  const rcState = await evaluate(`(() => {
    const rcOnly = KB_CATALOG.drivers.find((d) => d.versions.length && d.versions.every((v) => v.kind === 'rc'));
    const addOff = () => (rcOnly ? document.getElementById('add-' + rcOnly.name).disabled : null);
    const released = KB_CATALOG.kb_core.releases.some((r) => r.kind === 'release');
    const before = { released, toggle: document.getElementById('show-rc').checked, add: addOff(), kb: document.getElementById('kb-version').value,
      errors: document.querySelectorAll('.finding.error').length };
    document.getElementById('show-rc').click();
    const after = { toggle: document.getElementById('show-rc').checked, add: addOff(), hash: location.hash.includes('c=1') };
    document.getElementById('show-rc').click();
    return { before, after, back: document.getElementById('show-rc').checked === before.toggle };
  })()`);
  check('release-candidate switch: off once kb-core has a release, else on with no error; it shows and hides rc-only drivers',
    rcState.before.toggle === !rcState.before.released && rcState.before.kb !== '' && rcState.before.errors === 0
    && rcState.after.toggle === !rcState.before.toggle && rcState.after.add === !rcState.before.add
    && rcState.after.hash === rcState.after.toggle && rcState.back, rcState);
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
    tabs: document.querySelectorAll('#tabs [role=tab]').length,
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

  // The supply-chain panel: blockers inline next to the cause, a bus clash, and the cause button moves focus there.
  const blk = await evaluate(`(() => ({ boxes: document.querySelectorAll('#supply .blockers .finding.error').length,
    marked: [...document.querySelectorAll('#tray .tray-chip .blocker')].length,
    sbom: document.getElementById('sbom-spdx').disabled, zip: document.getElementById('panel-zip').disabled,
    sum: document.getElementById('supply-sum').textContent }))()`);
  check('panel: a bus clash shows as a blocker with its cause marked in the tray; the SBOM waits', blk.boxes >= 3 && blk.marked >= 2 && blk.sbom && blk.zip
    && /3 blockers/.test(blk.sum), blk);
  await click('.blockers .finding:nth-child(2) button.go');
  check('panel: "Go to cause" opens Advanced and focuses the bus', await evaluate(`document.getElementById('advanced').open && document.activeElement.id === 'bus-h-uart1'`));

  // The panel for the default selection, a driver change, stale and missing data, the details disclosure.
  await open('');
  const base = await evaluate(`(() => { const v = (id) => document.getElementById(id); return {
    sections: [...document.querySelectorAll('#supply .sc h3')].map((e) => e.textContent).join('|'),
    spdx: !v('sbom-spdx').disabled, cdx: !v('sbom-cdx').disabled, zip: !v('panel-zip').disabled,
    cmd: (v('verify-cmd') || {}).textContent, chip: document.querySelector('#h-cve').parentElement.querySelector('.badge').textContent,
    details: v('supply-details').open, perDetail: !!v('supply-details').textContent.match(/warning/), comps: v('cve-all').querySelectorAll('li').length,
    live: v('supply-sum').getAttribute('aria-live'), sum: v('supply-sum').textContent,
    inFiles: document.querySelectorAll('#advanced #tabs [role=tab]').length }; })()`);
  check('panel: SBOM, vulnerabilities, provenance and safety sections for the default selection', base.sections === 'SBOM|Known vulnerabilities|Provenance|Safety evidence'
    && base.spdx && base.cdx && base.zip && /^gh attestation verify kb-core-.*\.tar\.gz --repo kokeb-labs\/kb-core$/.test(base.cmd) && /None known/.test(base.chip), base);
  check('panel: details closed by default, summary in a live region, files tabs stay under Advanced', !base.details && base.perDetail && base.live === 'polite'
    && /components/.test(base.sum) && base.inFiles === 9, base);
  await click('#add-bmp581');
  const added = await evaluate(`({ comps: document.getElementById('cve-all').querySelectorAll('li').length, text: document.getElementById('cve-all').textContent,
    safe: [...document.querySelectorAll('#supply .evi li')].map((e) => e.textContent).join('|') })`);
  check('panel: adding a driver adds its component and its evidence row', added.comps === base.comps + 1 && /BMP581/.test(added.text) && /kb-core evidence/.test(added.safe), added);
  const stale = await evaluate(`(() => { const day = KB_VULNERABILITIES.checked_at; KB.panel.now = () => new Date('2099-01-01T00:00:00Z'); KB.ui.refresh();
    const r = { warn: (document.querySelector('#supply .warn-line') || {}).textContent, sum: document.getElementById('supply-sum').textContent };
    KB.panel.now = () => new Date(day + 'T12:00:00Z'); KB.ui.refresh();
    r.back = !document.querySelector('#supply .warn-line'); return r; })()`);
  check('panel: data older than 10 days shows a visible warning and clears when fresh', /days old/.test(stale.warn) && /days old/.test(stale.sum) && stale.back, stale);
  const gone = await evaluate(`(() => { const keep = KB_VULNERABILITIES; KB.panel.now = () => new Date(keep.checked_at + 'T12:00:00Z'); globalThis.KB_VULNERABILITIES = undefined; KB.ui.refresh();
    const r = { chip: document.querySelector('#h-cve').parentElement.querySelector('.badge').textContent, warn: (document.querySelector('#supply .warn-line') || {}).textContent,
      none: /None known/.test(document.getElementById('h-cve').parentElement.textContent) };
    globalThis.KB_VULNERABILITIES = keep; KB.ui.refresh(); return r; })()`);
  check('panel: missing vulnerability data is said, never "none known"', gone.chip === 'No data' && /missing/.test(gone.warn) && !gone.none, gone);

  // The page offers no ThreadX on ESP32, so the real ThreadX entry (8 reviewed advisories) is shown under the name of a
  // component the selection does hold.
  await open(example, {});
  const rev = await evaluate(`(() => { const keep = KB_VULNERABILITIES; const tx = keep.components.find((c) => c.name === 'threadx');
    KB.panel.now = () => new Date(keep.checked_at + 'T12:00:00Z');
    globalThis.KB_VULNERABILITIES = { ...keep, components: keep.components.map((c) => (c.name === 'esp-idf' ? { ...tx, name: 'esp-idf', version: c.version } : c)) }; KB.ui.refresh();
    const sec = document.getElementById('h-cve').parentElement; const d = sec.querySelector('details[id^=cve-rev-]');
    const r = { real: tx.reviewed + '/' + tx.open, chip: sec.querySelector('.badge').textContent, text: sec.textContent, d: d ? d.id : null, n: d ? d.querySelectorAll('li').length : 0,
      badge: d ? (d.querySelector('li .badge') || {}).textContent : null, title: d ? (d.querySelector('li .badge') || {}).title : null,
      href: d ? (d.querySelector('li a:last-of-type') || {}).href : null, sum: document.getElementById('supply-sum').textContent };
    globalThis.KB_VULNERABILITIES = keep; KB.ui.refresh(); return r; })()`);
  check('panel: reviewed advisories read "fixed in the pinned version", not open, with reason and source link',
    rev.real === '8/0' && rev.chip === '0 open, 8 reviewed' && rev.d === 'cve-rev-esp-idf' && rev.n === 8 && rev.badge === 'fixed in the pinned version'
    && /lists 6\.5\.2\.202603b as patched/.test(rev.title || '') && /security\/advisories\/GHSA-/.test(rev.href || '') && !/not built/i.test(rev.text)
    && /0 open vulnerabilities, 8 reviewed/.test(rev.sum), rev);

  await open('#v=1&f=stm32&t=stm32h7&b=nucleo-h743zi&r=threadx&n=planned', {});
  check('planned family and RTOS flagged', (await evaluate(`[...document.querySelectorAll('.finding.error code')].map(e => e.textContent)`)).includes('CFG-MCU-004'));

  await open(example, { width: 360, height: 780, mobile: true });
  check('no horizontal scroll at 360 px', await evaluate('document.documentElement.scrollWidth <= innerWidth + 1'),
    await evaluate('document.documentElement.scrollWidth'));
  await shot('06-phone-light');

  // The walkthrough of a first-time person: target, drivers, output. Real clicks, counted.
  await open('', { width: 1280, height: 900 });
  await s('Browser.setDownloadBehavior', { behavior: 'deny' });
  await evaluate(`window.__dl = []; const o = URL.createObjectURL; URL.createObjectURL = (b) => { window.__dl.push([b.size, b.type]); return o.call(URL, b); };`);
  clicks = 0;
  await click('label[for="fam-esp32"]'); // the target (already the default; clicked as a person would to confirm it)
  await click('#add-bmp581');
  await click('#add-bno08x');
  await click('#zip');
  const walkDl = await evaluate('window.__dl');
  check(`default path reaches Download in ${clicks} clicks (limit 4, never more than 6)`, clicks <= 4 && clicks <= 6
    && walkDl.length === 1 && walkDl[0][1] === 'application/zip' && walkDl[0][0] > 1000, { clicks, walkDl });
  check('the walkthrough selected two drivers and the tray shows them', await evaluate(`[...document.querySelectorAll('#tray .tray-chip')].length === 2
    && document.querySelectorAll('.card[data-selected="true"]').length === 2`));
  check('the pin meter and board select are gone from the main path; the board note is there', await evaluate(`!document.getElementById('board')
    && /board layer/.test(document.getElementById('board-note').textContent)`));
  check('the C++ standard starts at the family default', await evaluate(`document.getElementById('cxx').selectedOptions[0].textContent === 'C++20 (default)'`));
  check('the RTOS radios are the family allows only', await evaluate(`[...document.querySelectorAll('#rtos input')].map((i) => i.value).join() === 'freertos'`));
  await shot('09-walkthrough-light');

  // Tags: a chip filters with the count the catalogue gives; Reset clears it.
  await open('');
  const adc = await evaluate('KB_CATALOG.tag_index.tags.adc');
  await click('#tag-details summary');
  await click('#f-tag-adc');
  const tagged = await evaluate(`({ n: document.querySelectorAll('#catalogue .card').length, hash: location.hash, count: document.getElementById('count').textContent,
    chip: document.getElementById('f-tag-adc').textContent })`);
  check('a tag chip filters the drivers, with the catalogue count, and is in the link', tagged.n === adc && /&ft=adc/.test(tagged.hash)
    && /^adc\s*\d+$/.test(tagged.chip) && tagged.count.startsWith(`${adc} of `), tagged);
  await click('#filters-reset');
  check('Reset clears the tag filter', await evaluate(`document.querySelectorAll('#catalogue .card').length > ${adc} && !/ft=/.test(location.hash)`));

  // Unavailable drivers and the cascade: choosing the host drops what needs another family, says why, and Undo restores it.
  await open(example);
  await click('label[for="fam-host"]');
  const casc = await evaluate(`({ note: document.getElementById('cascade').hidden ? '' : document.getElementById('cascade-text').textContent,
    selected: [...document.querySelectorAll('#tray .tray-chip strong')].map((e) => e.textContent).sort().join(),
    toggle: document.getElementById('unavail-toggle').textContent, rtos: [...document.querySelectorAll('#rtos input')].map((i) => i.value).join() })`);
  check('a target change drops what no longer fits and says why', /ws2812_rmt removed: needs MCU family esp32/.test(casc.note) && /FreeRTOS is not available/.test(casc.note)
    && !/BMP581/i.test(casc.note) && /BMP581/.test(casc.selected) && casc.rtos === 'none,host', casc);
  check('"Show N unavailable" is offered', /^Show 1 unavailable$/.test(casc.toggle), casc.toggle);
  await click('#unavail-toggle');
  const un = await evaluate(`({ cards: document.querySelectorAll('#unavail .card').length, reason: (document.querySelector('#unavail .card .reason') || {}).textContent,
    disabled: !!document.querySelector('#unavail .card .add').disabled, hash: location.hash })`);
  check('the unavailable list shows the reason and cannot add', un.cards === 1 && /needs MCU family esp32/.test(un.reason) && un.disabled && /&su=1/.test(un.hash), un);
  await click('#cascade-undo');
  const undone = await evaluate(`({ selected: document.querySelectorAll('#tray .tray-chip').length, family: document.querySelector('input[name=family]:checked').value,
    hidden: document.getElementById('cascade').hidden })`);
  check('Undo restores the selection and the target', undone.selected === 4 && undone.family === 'esp32' && undone.hidden, undone);
  await click('label[for="fam-stm32"]');
  check('a planned family makes every driver unavailable, with one reason', await evaluate(`new RegExp('^(Show|Hide) ' + ${driverCount} + ' unavailable$').test(document.getElementById('unavail-toggle').textContent)
    && document.querySelectorAll('#catalogue .card').length === 0 && /no port/.test(document.querySelector('#unavail .reason').textContent)`));
  await click('#cascade-undo');

  // Views: cards, list, grouped; sort and group; remembered per viewer; the link reproduces the view.
  await open('');
  await click('[data-view="list"]');
  const list = await evaluate(`({ table: !!document.querySelector('#catalogue table.list'), rows: document.querySelectorAll('#catalogue tr.row-item').length,
    hash: location.hash, stored: localStorage.getItem('kb-configurator-view') })`);
  check('the list view is a table of every driver, remembered, and in the link', list.table && list.rows > 20 && /&vw=l/.test(list.hash) && list.stored === 'list', list);
  await click('#sort-h-bus');
  const sorted = await evaluate(`({ aria: document.querySelector('#sort-h-bus').closest('th').getAttribute('aria-sort'), hash: location.hash,
    first: document.querySelector('#catalogue tr.row-item th strong').textContent })`);
  check('a list header sorts', sorted.aria === 'ascending' && /&so=b/.test(sorted.hash), sorted);
  await open('#v=1&f=esp32&t=esp32s3&b=esp32s3-devkitc&r=freertos&k=&p=&n=x&d=&m=');
  check('the stored view is used when the link names none', await evaluate(`!!document.querySelector('#catalogue table.list')`));
  await click('[data-view="grouped"]');
  await evaluate(`(() => { const e = document.getElementById('group-by'); e.value = 'bus'; e.dispatchEvent(new Event('change')); })()`);
  await click('#fit');
  await evaluate(`(() => { const e = document.getElementById('sort-by'); e.value = 'category'; e.dispatchEvent(new Event('change')); })()`);
  const shape = `(() => ({ groups: [...document.querySelectorAll('#catalogue .group')].map((g) => g.getAttribute('aria-label') + ':' + [...g.querySelectorAll('h3')].map((h) => h.textContent).join('|')),
    view: document.querySelector('[data-view][aria-pressed=true]').dataset.view, group: document.getElementById('group-by').value, sort: document.getElementById('sort-by').value,
    fit: document.getElementById('fit').checked }))()`;
  const grouped = await evaluate(shape);
  const groupedHash = await evaluate('location.hash');
  check('grouped by bus with sort and filter together', grouped.view === 'grouped' && grouped.group === 'bus' && grouped.sort === 'category' && !grouped.fit
    && grouped.groups.length > 2 && /&vw=g&gb=b&so=c/.test(groupedHash) && /&fw=0/.test(groupedHash), { grouped, groupedHash });
  await evaluate(`localStorage.clear()`);
  await open(groupedHash);
  const again = await evaluate(shape);
  check('the link reproduces the view exactly', JSON.stringify(again) === JSON.stringify(grouped), { again, grouped });
  await shot('10-grouped-light');
  await open(groupedHash, { width: 360, height: 780, mobile: true, scheme: 'dark' });
  check('no horizontal scroll at 360 px in the grouped view', await evaluate('document.documentElement.scrollWidth <= innerWidth'), await evaluate('document.documentElement.scrollWidth'));
  await shot('11-phone-grouped-dark');
  await evaluate(`localStorage.clear()`);

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
