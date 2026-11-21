#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Exports the selections of export-matrix.json through the configurator, as a user would download them:
//
//   node tools/export_matrix.mjs OUT [--rows id,id] [--ci] [--chrome [PATH]]
//
// For every row it builds the selection (each driver at the catalogue's default version, release candidates on),
// runs the page's own code (the classic scripts and resolver.mjs, loaded as tests/load.mjs does) and writes every
// file of the Files panel, the starter project ZIP and its unpacked tree to OUT/<row>/. A row marked
// `expect_refused` must fail with that code and writes nothing else. With --chrome the same selection is also
// opened in headless Chrome (index.html served over http): every Download button of the Files panel, the ZIP
// button and the offline.html link are clicked, and each downloaded file must equal, byte for byte, what the page
// code gave in Node. OUT/summary.json lists each row's findings and files. Exit 1 on any difference or failure.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { load } from '../tests/load.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const conf = resolve(here, '..');
const args = process.argv.slice(2);
if (!args.length || args[0].startsWith('--')) {
  console.error('usage: export_matrix.mjs OUT [--rows id,id] [--ci] [--chrome [PATH]]');
  process.exit(2);
}
const out = resolve(args[0]);
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? (args[i + 1] && !args[i + 1].startsWith('--') ? args[i + 1] : true) : null; };
const matrix = JSON.parse(readFileSync(join(here, 'export-matrix.json'), 'utf8'));
let rows = matrix.rows;
if (opt('--rows')) { const want = String(opt('--rows')).split(','); rows = rows.filter((r) => want.includes(r.id)); }
if (opt('--ci')) rows = rows.filter((r) => r.ci);
if (!rows.length) { console.error('export_matrix: no row selected'); process.exit(2); }

const ctx = await load();
const { KB } = ctx;
const site = ctx.KB_CATALOG;
const byName = new Map(site.drivers.map((d) => [d.name, d]));

// The version a driver gets when added with release candidates shown: the newest version (ui.js latestVersion).
function newest(d) {
  return d.versions.slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))[0];
}

// Every driver of the catalogue on a bus of the row's target: I2C devices share i2c0 (moving to i2c1 on an
// address clash), SPI devices share spi0, each UART device gets its own port (point to point), CAN and ADC
// devices share can0 and adc0, PIO devices pio0. A driver that cannot run on the family (its MCU gate) is left out
// and named in the summary. Owner-task buses where a device needs it, guarded otherwise.
function everyDriver(row) {
  const fam = site.families.find((f) => f.id === row.family);
  const target = fam.targets.find((t) => t.id === row.target);
  const picked = [];
  const skipped = [];
  const used = { i2c0: new Set(), i2c1: new Set() };
  let uart = 0;
  for (const d of site.drivers) {
    const v = newest(d);
    if (!v) { skipped.push(`${d.name}: no version`); continue; }
    if (!v.mcus.includes('any') && !v.mcus.includes(row.family)) { skipped.push(`${d.name}: runs on ${v.mcus.join(', ')}`); continue; }
    const kind = v.bus[0] === 'modbus' ? 'uart' : v.bus[0];
    let bus;
    if (kind === 'i2c') {
      const addrs = d.i2c_addresses || [];
      bus = ['i2c0', 'i2c1'].find((b) => !addrs.length || addrs.some((a) => !used[b].has(a))) || 'i2c1';
      const free = addrs.find((a) => !used[bus].has(a));
      if (free !== undefined) used[bus].add(free);
    } else if (kind === 'uart') {
      if (uart >= target.buses.uart) { skipped.push(`${d.name}: no UART left`); continue; }
      bus = `uart${uart++}`;
    } else if (kind === 'canopen') bus = 'can0';
    else bus = `${kind}0`;
    picked.push([d.name, v.bus[0] === 'canopen' ? 'canopen' : kind, bus]);
  }
  return { picked, skipped };
}

function selection(row) {
  let list = row.drivers;
  let skipped = [];
  if (list === 'every') ({ picked: list, skipped } = everyDriver(row));
  const drivers = list.map(([name, via, bus]) => {
    const d = byName.get(name);
    if (!d) throw new Error(`${row.id}: ${name} is not in the catalogue`);
    return { name, version: newest(d).version, via, bus };
  });
  const buses = {};
  for (const d of drivers) {
    const own = byName.get(d.name).requires_owner;
    const want = (row.models || {})[d.bus];
    if (!buses[d.bus]) buses[d.bus] = { model: want || (own ? 'owner' : 'guarded') };
    if (own) buses[d.bus].model = 'owner';
  }
  const kb = site.kb_core.releases.slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))[0];
  const fam = site.families.find((f) => f.id === row.family);
  const port = fam && fam.port && fam.port.releases.length ? fam.port.releases[0].version : null;
  return {
    image: { image: row.image, kb_core: kb ? kb.version : null, port, mcu: { family: row.family, target: row.target, board: row.board },
      rtos: row.rtos, drivers, buses },
    skipped,
  };
}

const summary = { catalog_source_date: site.source_date, rows: [] };
const problems = [];
const expected = new Map();
rmSync(out, { recursive: true, force: true });
for (const row of rows) {
  const { image, skipped } = selection(row);
  const res = KB.shared.run(site, image);
  const gen = KB.generate.generate(site, image, res);
  const errors = res.findings.filter((f) => f.severity === 'error').map((f) => `${f.code}${f.mirrors ? '/' + f.mirrors : ''}`);
  const warnings = res.findings.filter((f) => f.severity === 'warning').map((f) => f.code);
  const entry = { id: row.id, build: row.build, hash: KB.state.encode(image, { rc: true }), drivers: image.drivers.length,
    skipped, errors, warnings, files: [] };
  summary.rows.push(entry);
  if (row.expect_refused) {
    const refused = res.shared && res.shared.errors.some((e) => e.code === row.expect_refused);
    entry.refused = refused ? row.expect_refused : null;
    if (!refused || gen.ready) problems.push(`${row.id}: expected the page to refuse it with ${row.expect_refused}`);
    console.log(`${refused ? 'ok  ' : 'FAIL'} ${row.id}: refused with ${row.expect_refused}`);
    continue;
  }
  if (errors.length || !gen.ready) { problems.push(`${row.id}: does not resolve: ${errors.join(', ')}`); console.log(`FAIL ${row.id}: ${errors.join(', ')}`); continue; }
  const dir = join(out, row.id);
  const files = new Map();
  for (const f of gen.files) files.set(f.path.split('/').pop(), Buffer.from(KB.core.utf8(f.text)));
  const zip = Buffer.from(KB.generate.zipProject(ctx.fflate, gen));
  files.set(`${KB.core.safeName(image.image)}.zip`, zip);
  mkdirSync(join(dir, 'downloads'), { recursive: true });
  for (const [name, bytes] of files) writeFileSync(join(dir, 'downloads', name), bytes);
  // The footer's offline copy is the committed offline.html (bundle_offline.py --check keeps it current).
  files.set('kb-configurator-offline.html', readFileSync(join(conf, 'offline.html')));
  // The unpacked project, with the executable bit kb-fetch.sh carries in the ZIP.
  const unz = ctx.fflate.unzipSync(new Uint8Array(zip));
  for (const [path, bytes] of Object.entries(unz)) {
    const p = join(dir, 'project', path.split('/').slice(1).join('/'));
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, Buffer.from(bytes), { mode: path.endsWith('.sh') ? 0o755 : 0o644 });
  }
  entry.files = [...files.keys()].sort();
  expected.set(row.id, { files, hash: entry.hash });
  console.log(`ok   ${row.id}: ${image.drivers.length} driver(s), ${files.size} files, warnings ${[...new Set(warnings)].join(' ')}${skipped.length ? `; left out: ${skipped.join('; ')}` : ''}`);
}

if (opt('--chrome') && expected.size) await viaChrome(typeof opt('--chrome') === 'string' ? opt('--chrome') : null);
mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'summary.json'), JSON.stringify(summary, null, 2) + '\n');
if (problems.length) { console.log(problems.map((p) => `FAIL ${p}`).join('\n')); process.exit(1); }
console.log(`export_matrix: ${summary.rows.length} row(s) passed${opt('--chrome') ? ' (Node and Chrome downloads agree)' : ''}`);

// -- the same downloads from the page in headless Chrome ----------------------------------------------------------
async function viaChrome(path) {
  const chrome = [path, process.env.CHROME, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'].filter(Boolean).find((p) => existsSync(p));
  if (!chrome) { problems.push('--chrome: no Chrome or Chromium found'); return; }
  const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.json': 'application/json' };
  const server = createServer((req, res) => {
    const p = resolve(conf, '.' + decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (!p.startsWith(conf) || !existsSync(p) || !statSync(p).isFile()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': types[p.slice(p.lastIndexOf('.'))] || 'application/octet-stream' });
    res.end(readFileSync(p));
  });
  await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
  const base = `http://127.0.0.1:${server.address().port}/index.html`;
  const profile = mkdtempSync(join(tmpdir(), 'kb-export-'));
  const proc = spawn(chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run',
    '--no-default-browser-check', '--disable-extensions', '--disable-gpu', '--disable-dev-shm-usage', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
  const ws = await new Promise((ok, fail) => {
    let buf = '';
    const t = setTimeout(() => fail(new Error('Chrome did not start')), 60000);
    proc.stderr.on('data', (d) => { buf += d; const m = /DevTools listening on (ws:\/\/\S+)/.exec(buf); if (m) { clearTimeout(t); ok(m[1]); } });
  });
  const sock = new WebSocket(ws);
  await new Promise((ok) => sock.addEventListener('open', ok, { once: true }));
  let seq = 0;
  const pending = new Map();
  const events = [];
  sock.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); if (m.error) p.fail(new Error(m.error.message)); else p.ok(m.result); } else if (m.method) events.push(m);
  });
  const send = (method, params = {}, sessionId) => new Promise((ok, fail) => { const id = ++seq; pending.set(id, { ok, fail }); sock.send(JSON.stringify({ id, method, params, sessionId })); });
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const s = (m, p) => send(m, p, sessionId);
  const evaluate = async (expr) => {
    const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || 'evaluate failed');
    return r.result.value;
  };
  await s('Page.enable');
  try {
    for (const [id, exp] of expected) {
      const dl = join(out, id, 'chrome');
      mkdirSync(dl, { recursive: true });
      await send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: dl, eventsEnabled: true });
      await s('Page.navigate', { url: base + exp.hash });
      await sleep(400);
      await s('Page.reload', { ignoreCache: true });
      await sleep(1500);
      const tabs = await evaluate('[...document.querySelectorAll("#tabs [role=tab]")].map((t) => t.id)');
      for (const tab of tabs) {
        await evaluate(`document.getElementById(${JSON.stringify(tab)}).click()`);
        await evaluate('document.getElementById("save-file").click()');
        await sleep(250);
      }
      await evaluate('document.getElementById("zip").click()');
      await evaluate('document.getElementById("offline-link").click()');
      await sleep(800);
      for (let i = 0; i < 40 && readdirSync(dl).filter((n) => !n.endsWith('.crdownload')).length < exp.files.size; i++) await sleep(250);
      const got = new Set(readdirSync(dl).filter((n) => !n.endsWith('.crdownload')));
      let same = 0;
      for (const [name, bytes] of exp.files) {
        if (!got.has(name)) { problems.push(`${id}: Chrome did not download ${name}`); continue; }
        if (!readFileSync(join(dl, name)).equals(bytes)) problems.push(`${id}: ${name} from Chrome differs from the page code in Node`);
        else same++;
      }
      const row = summary.rows.find((r) => r.id === id);
      row.chrome = { downloaded: [...got].sort(), identical: same };
      console.log(`${same === exp.files.size ? 'ok  ' : 'FAIL'} ${id}: Chrome downloaded ${got.size} file(s), ${same} identical`);
    }
  } finally {
    sock.close();
    proc.kill('SIGKILL');
    server.close();
    rmSync(profile, { recursive: true, force: true });
  }
}
