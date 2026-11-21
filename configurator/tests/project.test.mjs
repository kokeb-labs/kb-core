// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// "Open an existing project" and the upgrade view: the YAML subset parser, file classification, the selection
// built from an image and its lock, and the shared upgrade planner (resolver.mjs plan) against kb-core's upgrade
// vectors (tests/resolver/vectors/upgrade, the vectors tools/kb/tests/test_upgrade.py runs through kb upgrade).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { load, root } from './load.mjs';

const ctx = await load();
const { KB } = ctx;
const P = KB.project;
const plain = (v) => JSON.parse(JSON.stringify(v));
const upgradeDir = process.env.KB_SHARED_VECTORS ? join(process.env.KB_SHARED_VECTORS, 'upgrade')
  : join(root, '..', 'tests', 'resolver', 'vectors', 'upgrade');
const upgradeFiles = existsSync(upgradeDir) ? readdirSync(upgradeDir).filter((n) => n.endsWith('.json')).sort() : [];

test('the YAML subset reads flow, block and quoted styles as PyYAML does', () => {
  const doc = P.parseYaml([
    '# a hand-written image', 'schema: 1', 'image: cli-board   # trailing comment',
    'kb_core: {version: 0.4.0}', "mcu: {family: esp32, target: 'esp32c6'}",
    'drivers: [pcal9555, {name: tmc5160, version: "0.1.0-rc.1"}]',
    'buses:', '  i2c0: {model: guarded, devices: [pcal9555]}', '  spi1:', '    model: owner', '    devices:', '    - tmc5160',
  ].join('\n'));
  assert.deepEqual(plain(doc), {
    schema: 1, image: 'cli-board', kb_core: { version: '0.4.0' }, mcu: { family: 'esp32', target: 'esp32c6' },
    drivers: ['pcal9555', { name: 'tmc5160', version: '0.1.0-rc.1' }],
    buses: { i2c0: { model: 'guarded', devices: ['pcal9555'] }, spi1: { model: 'owner', devices: ['tmc5160'] } },
  });
  assert.deepEqual(plain(P.parseYaml('drivers:\n  - name: bmp581\n    version: 0.2.0\n  - pf1550\nrtos: ~\nn: 1.5\nb: yes\n')),
    { drivers: [{ name: 'bmp581', version: '0.2.0' }, 'pf1550'], rtos: null, n: 1.5, b: true });
  assert.deepEqual(plain(P.parseYaml('a: "x # not a comment"\nb: \'it\'\'s\'\n')), { a: 'x # not a comment', b: "it's" });
  // the forms PyYAML resolves the same way: -.5 stays a string, .5 and 1. are floats, 1_000 an integer, a quoted
  // date a string, and a comment may follow a closing quote directly
  assert.deepEqual(plain(P.parseYaml('a: -.5\nb: .5\nc: 1.\nd: 1_000\ne: "2026-11-14"\nf: "x"# c\ng: 1.0e5\n')),
    { a: '-.5', b: 0.5, c: 1, d: 1000, e: '2026-11-14', f: 'x', g: '1.0e5' });
});

test('the YAML subset refuses what it does not support', () => {
  for (const bad of ['a: 010\n', 'a: 0x1F\n', 'a: 0b101\n', 'a: 1:30\n', 'a: .inf\n', 'a: .NaN\n', 'image: 2026-11-14\n',
    '1: a\n', 'a: {1: x}\n', 'true: x\n', 'a:\tb\n', 'a: 1\n...\nb: 2\n', '__proto__:\n  mcu: {family: esp32}\n',
    '{__proto__: 1}\n', 'a: [{__proto__: 1}]\n', '<<: {a: 1}\n',
    'a: &x 1\nb: *x\n', 'a: !!str 1\n', 'a: |\n  text\n', 'a: 1\na: 2\n', '---\na: 1\n---\nb: 2\n',
    'a:\n\tb: 1\n', 'a: b: c\n', 'a: [1, 2\n', 'a: "open\n', 'a: 1\n  b: 2\n']) {
    assert.throws(() => P.parseYaml(bad), P.YamlError, JSON.stringify(bad));
  }
  assert.throws(() => P.parseYaml('a: ' + 'x'.repeat(300 * 1024)), /256 KiB/);
  assert.throws(() => P.parseYaml('a: ' + '['.repeat(40) + ']'.repeat(40)), /deeply/);
});

test('every upgraded kb-image.yml reads back as its image', { skip: upgradeFiles.length ? false : 'no upgrade vectors' }, () => {
  for (const name of upgradeFiles) {
    const vec = JSON.parse(readFileSync(join(upgradeDir, name), 'utf8'));
    if (!vec.expect.ok) continue;
    assert.deepEqual(plain(P.parseYaml(vec.expect.files['kb-image.yml'])), vec.expect.image, name);
    assert.deepEqual(plain(P.parseDocument(vec.expect.files['kb-image.lock.yml'])), vec.expect.lock, name);
  }
});

test('the site\'s own kb-image.yml download reads back', () => {
  const sel = {
    image: 'weather-node', kb_core: KB_CATALOG().kb_core.releases[0].version, port: null, rtos: 'freertos',
    mcu: { family: 'esp32', target: 'esp32s3', board: 'esp32s3-devkitc' },
    drivers: [{ name: 'bmp581', version: KB_CATALOG().drivers.find((d) => d.name === 'bmp581').versions[0].version, via: 'i2c', bus: 'i2c0' }],
    buses: { i2c0: { model: 'guarded' } },
  };
  const res = KB.shared.run(KB_CATALOG(), sel);
  const out = KB.generate.generate(KB_CATALOG(), sel, res);
  const text = out.files.find((f) => f.id === 'image').text;
  assert.deepEqual(plain(P.parseYaml(text)), plain(res.shared.image));
});

function KB_CATALOG() {
  return ctx.KB_CATALOG;
}

test('files are told apart: image, lock, and neither', () => {
  const lock = '# kb image lock (schema 1), written by kb resolve: do not edit.\n' + JSON.stringify({
    schema: 1, image: 'x', kb_core: { version: '0.4.0', commit: 'a'.repeat(40) }, ports: [], drivers: [] });
  const got = P.classify([{ name: 'kb-image.lock.yml', text: lock }, { name: 'kb-image.yml', text: 'schema: 1\nmcu: {family: host}\n' },
    { name: 'notes.yml', text: 'hello: world\n' }, { name: 'broken.yml', text: 'a: [\n' }, { name: 'again.yml', text: 'mcu: {family: esp32}\n' }]);
  assert.equal(got.lock.name, 'kb-image.lock.yml');
  assert.equal(got.image.name, 'kb-image.yml');
  assert.equal(got.problems.length, 3, got.problems.join('; '));
});

test('a lock alone gives back its image', () => {
  const vec = JSON.parse(readFileSync(join(upgradeDir, '001-upgrade-with-lock.json'), 'utf8'));
  const img = P.imageFromLock(vec.lock);
  assert.equal(img.kb_core.version, vec.lock.kb_core.version);
  assert.deepEqual(plain(img.drivers.map((d) => d.name)), vec.lock.drivers.map((d) => d.name));
  const catalog = JSON.parse(readFileSync(join(upgradeDir, vec.catalog), 'utf8'));
  const a = KB.engine.plan(catalog, img, { lock: vec.lock });
  assert.ok(a.ok, JSON.stringify(a.errors));
  assert.deepEqual(plain(a.rows), vec.expect.rows);
});

test('an opened image and lock become the page selection', () => {
  const site = KB_CATALOG();
  const image = { schema: 1, image: 'My Board', kb_core: { version: '0.0.0' }, mcu: { family: 'esp32', target: 'esp32c6' },
    drivers: ['bmp581', { name: 'tmc5160' }, { name: 'nosuch', version: '1.0.0' }],
    buses: { i2c0: { model: 'guarded', devices: ['bmp581'] }, uart1: { model: 'owner', kind: 'uart', devices: ['tmc5160'] } } };
  const lock = { schema: 1, kb_core: { version: '0.0.0' }, drivers: [{ name: 'bmp581', version: '0.1.0-rc.1' }, { name: 'tmc5160', version: '0.1.0-rc.1' }], ports: [] };
  const { selection, notes } = P.toSelection(site, image, lock);
  assert.equal(selection.image, 'my-board');
  assert.deepEqual(plain(selection.drivers), [
    { name: 'bmp581', version: '0.1.0-rc.1', via: 'i2c', bus: 'i2c0' },
    { name: 'tmc5160', version: '0.1.0-rc.1', via: 'uart', bus: 'uart1' }]);
  assert.deepEqual(plain(selection.buses), { i2c0: { model: 'guarded' }, uart1: { model: 'owner' } });
  assert.ok(notes.some((n) => /nosuch .*left out/.test(n)), 'a driver the catalogue lacks is left out, not kept');
  // the hash of the opened selection carries catalogue identifiers only, as for any selection
  assert.ok(!/My%20Board|My Board/.test(KB.state.encode(selection)));
});

test('the site catalogue carries known-vulnerability scans into the planner', () => {
  const cat = KB.shared.kbCatalog(KB_CATALOG());
  for (const d of cat.drivers) for (const v of d.versions) assert.ok(v.osv === null || typeof v.osv === 'object');
  const d = KB_CATALOG().drivers.find((x) => x.versions.length);
  const img = { schema: 1, image: 'scan', kb_core: { version: cat.kb_core.versions[0].version }, mcu: { family: 'host' },
    drivers: [{ name: d.name, version: d.versions[0].version }] };
  const p = KB.engine.plan(cat, img, {});
  assert.ok(p.ok, JSON.stringify(p.errors));
  assert.equal(p.rows.length, 2);
  const scan = d.versions[0].osv;
  assert.deepEqual(plain(p.rows[1].osv_from), scan ? [...new Set(scan.findings.map((f) => f.id))].sort() : null);
});

test('upgrade vectors through the site engine', { skip: upgradeFiles.length ? false : `none at ${upgradeDir}` }, () => {
  assert.ok(upgradeFiles.length >= 15);
  for (const name of upgradeFiles) {
    const vec = JSON.parse(readFileSync(join(upgradeDir, name), 'utf8'));
    const catalog = typeof vec.catalog === 'string' ? JSON.parse(readFileSync(join(upgradeDir, vec.catalog), 'utf8')) : vec.catalog;
    const opts = vec.options || {};
    const got = KB.engine.plan(catalog, vec.image, { lock: vec.lock, to: opts.to, allowRc: opts.allow_rc === true });
    const pick = (l) => l.map((e) => ({ code: e.code, subject: e.subject }));
    const actual = { ok: got.ok, errors: pick(got.errors), warnings: pick(got.warnings), rows: got.rows, image: got.image, lock: got.lock };
    if (got.ok) actual.files = KB.engine.upgradeFiles(got, opts.allow_rc === true);
    assert.equal(KB.engine.canonicalJson(actual), KB.engine.canonicalJson(vec.expect), name);
  }
});

test('the reader stays fast at the size cap (linear work, capped lines)', () => {
  const cases = [
    'a: [\n' + 'x,\n'.repeat(80000) + ']\n',                 // a flow collection over many lines: refused by the caps
    'a: [' + 'x,'.repeat(120000) + 'x]\n',                     // one long flow line
    'drivers:\n' + '  - name: x\n'.repeat(9000),              // a long block sequence of mappings
    'a: ' + '"' + 'x'.repeat(250000) + '"\n',                  // one long quoted string
  ];
  for (const text of cases) {
    assert.ok(text.length <= 256 * 1024 + 16, String(text.length));
    const t0 = process.hrtime.bigint();
    try { P.parseYaml(text); } catch (e) { assert.ok(e instanceof P.YamlError, String(e)); }
    const ms = Number(process.hrtime.bigint() - t0) / 1e6;
    assert.ok(ms < 1000, `${ms.toFixed(0)} ms for a ${text.length}-byte document`);
  }
  assert.throws(() => P.parseYaml('a: [\n' + 'x,\n'.repeat(600) + ']\n'), /spans more than 500 lines/);
});

test('a lock-only project ignores malformed buses and odd kinds without a RegExp from the file', () => {
  const img = P.imageFromLock({ schema: 1, image: 'x', kb_core: { version: '0.0.0', commit: 'a'.repeat(40) },
    mcu: { family: 'host', target: 'host' }, ports: [], drivers: [null, { name: 'bmp581', version: '0.1.0-rc.1' }],
    buses: [{ name: 'uart1', kind: '(a+)+$', devices: ['bmp581'] }, { name: 'i2c0', kind: 'i2c' }, null,
      { name: 'uart2', kind: 'modbus', devices: ['x', 3] }] });
  assert.deepEqual(plain(img.buses), { uart1: { devices: ['bmp581'] }, i2c0: { devices: [] },
    uart2: { kind: 'modbus', devices: ['x'] } });
  assert.deepEqual(plain(img.drivers), [{ name: 'bmp581', version: '0.1.0-rc.1' }]);
});
