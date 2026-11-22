// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Static checks of the page without a browser: the Content Security Policy, integrity hashes, no inline code, no
// network access in the scripts, the offline bundle up to date, licence headers, and the wording rules.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { root } from './load.mjs';

const html = readFileSync(join(root, 'index.html'), 'utf8');
const py = (args) => spawnSync('python3', args, { cwd: root, encoding: 'utf8' });

test('a strict Content Security Policy', () => {
  const csp = /http-equiv="Content-Security-Policy" content="([^"]+)"/.exec(html)[1];
  const d = Object.fromEntries(csp.split(';').map((s) => s.trim().split(/\s+/)).map(([k, ...v]) => [k, v]));
  assert.deepEqual(d['default-src'], ["'none'"]);
  assert.deepEqual(d['script-src'], ["'self'"]);
  assert.deepEqual(d['style-src'], ["'self'"]);
  assert.deepEqual(d['connect-src'], ["'none'"]);
  assert.deepEqual(d['base-uri'], ["'none'"]);
  assert.deepEqual(d['form-action'], ["'none'"]);
  assert.ok(!/unsafe-|data:|https?:/.test(csp), csp);
});

test('no inline script, style or handler in the page', () => {
  assert.ok(!/<script(?![^>]*\bsrc=)[^>]*>/.test(html), 'inline script');
  assert.ok(!/<style/.test(html), 'inline style');
  assert.ok(!/\sstyle="/.test(html), 'style attribute');
  assert.ok(!/\son[a-z]+="/.test(html), 'event handler attribute');
});

test('every script and the stylesheet carry a current sha384 integrity hash', () => {
  const r = py(['tools/sri.py', '--check']);
  assert.equal(r.status, 0, r.stderr + r.stdout);
});

test('offline.html is the same page, inlined, and up to date', () => {
  const r = py(['tools/bundle_offline.py', '--check']);
  assert.equal(r.status, 0, r.stdout);
  const off = readFileSync(join(root, 'offline.html'), 'utf8');
  const csp = /http-equiv="Content-Security-Policy" content="([^"]+)"/.exec(off)[1];
  assert.ok(!/unsafe-/.test(csp));
  const blocks = (off.match(/<script>/g) || []).length + (off.match(/<style>/g) || []).length;
  assert.equal((csp.match(/'sha384-/g) || []).length, blocks);
  assert.ok(!/<script[^>]*\bsrc=|type="module"|modulepreload/.test(off), 'nothing loaded from a file');
});

function walk(dir, out = []) {
  for (const n of readdirSync(dir)) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}
const files = walk(root).filter((p) => !p.includes(`${join('assets', 'figures')}`));
const own = files.filter((p) => /\.(js|mjs|py|css)$/.test(p) && !p.includes('vendor') && !p.endsWith(join('data', 'catalog.js')));
const licensed = (p) => readFileSync(p, 'utf8').split('\n').slice(0, 8).join('\n');

test('the page scripts never touch the network', () => {
  for (const p of own.filter((f) => f.includes(join('configurator', 'js')) || f.endsWith('resolver.mjs'))) {
    const src = readFileSync(p, 'utf8');
    assert.ok(!/\bfetch\(|XMLHttpRequest|WebSocket|sendBeacon|import\(|new Worker|eval\(|new Function|document\.cookie|innerHTML/.test(src), relative(root, p));
  }
});

test('every source file names its licence and the copyright holder', () => {
  for (const p of own) {
    const head = licensed(p);
    assert.match(head, /SPDX-License-Identifier: MIT/, relative(root, p));
    assert.match(head, /Copyright \(c\) 2026 The kb-core authors/, relative(root, p));
  }
});

test('wording rules: one public address, no names of people, products or tools', () => {
  const banned = [['hard', 'foc'], ['pw', '-controller'], ['clau', 'de'], ['anthro', 'pic'], ['co-auth', 'ored'], ['nebi', 'yu'], ['n3b', '3x'], ['generated ', 'with']]
    .map(([a, b]) => new RegExp(a + b, 'i'));
  for (const p of files.filter((f) => /\.(js|mjs|py|css|html|md|json|svg)$/.test(f) && !f.includes('vendor'))) {
    const text = readFileSync(p, 'utf8');
    for (const re of banned) assert.ok(!re.test(text), `${relative(root, p)} matches ${re}`);
    for (const m of text.matchAll(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[a-z]{2,}/g)) {
      assert.ok(['kokeblabs@gmail.com', 'noreply@github.com'].includes(m[0]) || /example\.com$/.test(m[0]), `${relative(root, p)}: ${m[0]}`);
    }
  }
});

test('the main path is target, drivers, output; everything else sits in one closed Advanced disclosure', () => {
  assert.match(html, /<meta name="description" content="[^"]{40,}">/);
  const main = html.slice(html.indexOf('<section class="panel" id="step-platform"'), html.indexOf('id="advanced"'));
  assert.ok(!/id="(board|pins|kb-version|port-version|image-name|show-rc|open-files|tabs|explore)"/.test(main), 'advanced control on the main path');
  assert.deepEqual([...main.matchAll(/<section class="panel" id="(step-[a-z]+)"/g)].map((m) => m[1]), ['step-platform', 'step-drivers', 'step-output']);
  assert.equal((html.match(/<details[^>]*id="advanced"[^>]*>/g) || []).length, 1);
  assert.ok(!/<details[^>]*id="advanced"[^>]* open/.test(html));
  const adv = html.slice(html.indexOf('id="advanced"'), html.indexOf('</details>', html.indexOf('id="advanced"')));
  for (const id of ['open-files', 'kb-version', 'port-version', 'image-name', 'show-rc', 'step-buses', 'explore', 'tabs']) assert.ok(adv.includes(`id="${id}"`), id);
});

test('the page holds no driver, target or board rule of its own', () => {
  const src = ['picker', 'ui', 'upgrade'].map((f) => readFileSync(join(root, 'js', f + '.js'), 'utf8')).join('\n');
  const data = JSON.parse(readFileSync(join(root, 'data', 'catalog.json'), 'utf8'));
  const words = [...data.drivers.map((d) => d.name), ...data.families.flatMap((f) => f.targets.map((t) => t.id))].filter((w) => w.length > 3);
  for (const w of words) assert.ok(!new RegExp(`['"\`]${w}['"\`]`).test(src), `js names ${w}`);
});
