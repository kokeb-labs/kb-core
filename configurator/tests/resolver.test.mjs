// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The site checks against their own vectors (tests/vectors/resolver), the shared resolver against the shared
// vectors of kb-core (tests/resolver/vectors at the repository root, or KB_SHARED_VECTORS), and the two against
// each other: the site never accepts what the shared rules refuse.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { load, root } from './load.mjs';

const ctx = await load();
const own = join(root, 'tests', 'vectors', 'resolver');
const shared = process.env.KB_SHARED_VECTORS || join(root, '..', 'tests', 'resolver', 'vectors');
const json = (p) => JSON.parse(readFileSync(p, 'utf8'));

function subset(actual, expected, path) {
  if (expected === null || typeof expected !== 'object' || Array.isArray(expected)) {
    assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected, path);
    return;
  }
  for (const k of Object.keys(expected)) subset(actual ? actual[k] : undefined, expected[k], `${path}.${k}`);
}

for (const file of readdirSync(own).filter((f) => f.endsWith('.json')).sort()) {
  const vec = json(join(own, file));
  const catalog = json(join(own, vec.catalog));
  test(`site vector ${file}`, () => {
    const r = ctx.KB.resolver.resolve(catalog, JSON.parse(JSON.stringify(vec.image)));
    assert.equal(r.ok, vec.expect.ok, 'ok');
    assert.deepEqual([...r.errors], vec.expect.errors, 'errors');
    assert.deepEqual([...r.warnings], vec.expect.warnings, 'warnings');
    if (vec.expect.infos) assert.deepEqual([...r.infos], vec.expect.infos, 'infos');
    if (vec.expect.resolved) subset(r.resolved, vec.expect.resolved, 'resolved');
    for (const f of r.findings) assert.ok(f.message.length > 10 && !/undefined|\[object|NaN/.test(f.message), `plain message for ${f.code}: ${f.message}`);
  });
  test(`site and shared rules agree on ${file}`, () => {
    const both = ctx.KB.shared.run(catalog, JSON.parse(JSON.stringify(vec.image)));
    const s = both.shared;
    if (!s.ok) assert.equal(both.ok, false, `shared refuses (${s.errors.map((e) => e.code).join(', ')}) but the site would offer the download`);
    // Every shared refusal a site check mirrors is reported by that check.
    const mirrors = new Set(Object.values(ctx.KB.resolver.MIRRORS));
    const site = new Set(both.findings.filter((f) => !f.shared).map((f) => f.mirrors).filter(Boolean));
    const unexplained = s.errors.filter((e) => mirrors.has(e.code) && !site.has(e.code) && !both.findings.some((f) => f.shared && f.code === e.code));
    assert.deepEqual(unexplained, []);
    if (s.ok) {
      // The lock the page offers is exactly the shared resolver's.
      const out = ctx.KB.generate.generate(catalog, vec.image, both);
      assert.equal(out.files.find((f) => f.id === 'lock').text, s.files['kb-image.lock.yml']);
    }
  });
}

const sharedFiles = existsSync(shared) ? readdirSync(shared).filter((f) => f.endsWith('.json')).sort() : [];
test('shared vectors present', { skip: sharedFiles.length ? false : `none at ${shared} (kb-core's tests/resolver/vectors/ lands with the kb tool)` }, () => {});
for (const file of sharedFiles) {
  test(`shared vector ${file} through resolver.mjs`, () => {
    const vec = json(join(shared, file));
    const catalog = typeof vec.catalog === 'string' ? json(join(dirname(join(shared, file)), vec.catalog)) : vec.catalog;
    const engine = ctx.KB.engine;
    if ('selection' in vec) {
      assert.equal(engine.canonicalJson(engine.availability(catalog, vec.selection)), engine.canonicalJson(vec.expect));
      return;
    }
    const got = engine.resolve(catalog, vec.image);
    const pick = (list) => list.map((e) => ({ code: e.code, subject: e.subject, ...("alternative" in e ? { alternative: e.alternative } : {}) }));
    const actual = { ok: got.ok, errors: pick(got.errors), warnings: pick(got.warnings) };
    if (got.ok) {
      actual.lock = got.lock;
      actual.files = engine.files(got.lock);
    }
    assert.equal(engine.canonicalJson(actual), engine.canonicalJson(vec.expect));
  });
}

test('every site code in the specification is exercised, and none is used unspecified', () => {
  const spec = readFileSync(join(root, '..', 'docs', 'configurator-checks.md'), 'utf8');
  const codes = [...new Set([...spec.matchAll(/^\| `(CFG-[A-Z]+-\d{3})` \|/gm)].map((m) => m[1]))];
  assert.ok(codes.length > 30, 'the specification lists the codes');
  const seen = new Set();
  for (const f of readdirSync(own)) {
    const v = json(join(own, f));
    for (const c of [...v.expect.errors, ...v.expect.warnings, ...(v.expect.infos || [])]) seen.add(c);
  }
  const extra = new Set(['CFG-BUS-013', 'CFG-GPIO-003', 'CFG-DRV-012', 'CFG-PORT-004']); // unit tests below and the sample catalogue
  assert.deepEqual(codes.filter((c) => !seen.has(c) && !extra.has(c)), []);
  assert.deepEqual([...seen].filter((c) => !codes.includes(c)), [], 'codes used but not specified');
  const src = readFileSync(join(root, 'js', 'resolver.js'), 'utf8');
  const inCode = new Set([...src.matchAll(/'(CFG-[A-Z]+-\d{3})'/g)].map((m) => m[1]));
  assert.deepEqual([...inCode].filter((c) => !codes.includes(c)), [], 'codes in resolver.js but not specified');
  for (const [site, kb] of Object.entries(ctx.KB.resolver.MIRRORS)) {
    assert.match(spec, new RegExp('\\| `' + site + '` \\|[^\\n]*\\| `' + kb + '` \\|'), `${site} mirrors ${kb} in the specification`);
  }
});

test('ADC channel budget (CFG-BUS-013) and kb_core_min note (CFG-DRV-012)', () => {
  const catalog = json(join(root, 'tests', 'vectors', 'catalog-fixture.json'));
  catalog.families.find((f) => f.id === 'esp32').targets.find((t) => t.id === 'esp32c3').buses.adc = 1;
  const image = { image: 'x', kb_core: null, port: null, mcu: { family: 'esp32', target: 'esp32c3', board: 'esp32c3-devkitm' }, rtos: 'freertos',
    drivers: [{ name: 'mcp9700', version: '0.1.0', via: 'adc', bus: 'adc0' }, { name: 'ntc_thermistor', version: '0.1.0', via: 'adc', bus: 'adc0' },
      { name: 'pf1550', version: '0.2.0', via: 'i2c', bus: 'i2c0' }], buses: { adc0: { model: 'guarded' }, i2c0: { model: 'guarded' } } };
  const r = ctx.KB.resolver.resolve(catalog, image);
  assert.ok(r.errors.includes('CFG-BUS-013'));
  assert.ok(r.infos.includes('CFG-DRV-012'));
});

test('the sample catalogue resolves a first selection with warnings and notes only, in both rule sets', () => {
  const cat = ctx.KB_CATALOG;
  const bmp = cat.drivers.find((d) => d.name === 'bmp581');
  const image = { image: 'my-image', kb_core: cat.kb_core.releases[0].version, port: null, mcu: { family: 'esp32', target: 'esp32s3', board: 'generic-esp32s3' }, rtos: 'freertos',
    drivers: [{ name: 'bmp581', version: ctx.KB.resolver.latestVersion(bmp).version, via: 'i2c', bus: 'i2c0' }], buses: { i2c0: { model: 'guarded' } } };
  const r = ctx.KB.shared.run(cat, image);
  assert.equal(r.ok, true, JSON.stringify(r.findings));
  assert.ok(r.infos.includes('CFG-GPIO-003'));
  // The ESP32 port is published (an rc), so it is pinned and warned about like a driver, not noted as inside kb-core.
  assert.ok(!r.infos.includes('CFG-PORT-004'), 'the ESP32 port has its own release');
  assert.equal(r.shared.ok, true);
  // kb-core warns as a release candidate (W01) or, before its first tag, as a development revision (W03).
  const kbWarn = cat.kb_core.releases[0].kind === 'dev' ? 'KB-RES-W03' : 'KB-RES-W01';
  assert.deepEqual(r.shared.warnings.map((w) => w.code).sort(), ['KB-RES-W01', 'KB-RES-W01', kbWarn].sort());
});

test('address assignment search', () => {
  const a = ctx.KB.resolver.assignAddresses([{ name: 'a', addresses: [1, 2] }, { name: 'b', addresses: [1] }, { name: 'c', addresses: [2, 3] }]);
  assert.deepEqual(JSON.parse(JSON.stringify(a)), { a: 2, b: 1, c: 3 });
  assert.equal(ctx.KB.resolver.assignAddresses([{ name: 'a', addresses: [1] }, { name: 'b', addresses: [1] }]), null);
});
