// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The language floor on the page (docs/language-floor.md): the C++ standard in the link, in kb-image.yml and back
// from an opened project; the catalogue's language fields reach the shared resolver; a part above the declared
// standard is refused with the nearest version that fits.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { load, root } from './load.mjs';

const ctx = await load();
const { KB } = ctx;
const fixture = JSON.parse(readFileSync(join(root, 'tests', 'vectors', 'catalog-fixture.json'), 'utf8'));
const plain = (v) => JSON.parse(JSON.stringify(v));
const image = (extra = {}) => ({
  image: 'demo', kb_core: '0.4.0', port: '0.4.0', rtos: 'freertos',
  mcu: { family: 'esp32', target: 'esp32s3', board: 'esp32s3-devkitc' },
  drivers: [{ name: 'bmp581', version: '0.1.0', via: 'i2c', bus: 'i2c0' }],
  buses: { i2c0: { model: 'guarded' } },
  ...extra,
});

test('the link carries the declared standard, and only then', () => {
  const withStd = KB.state.encode(image({ cxx: 23 }));
  assert.match(withStd, /&x=23(&|$)/);
  assert.equal(KB.state.decode(withStd).cxx, 23);
  const without = KB.state.encode(image());
  assert.ok(!/[&#]x=/.test(without), without);
  assert.equal('cxx' in KB.state.decode(without), false);
  assert.equal('cxx' in KB.state.decode(`${without}&x=99`), false, 'an unknown standard is dropped');
});

test('kb-image.yml gets a language block when a standard is declared', () => {
  const r = KB.shared.run(fixture, image({ cxx: 20 }));
  const out = KB.generate.generate(fixture, image({ cxx: 20 }), r);
  const yml = out.files.find((f) => f.id === 'image').text;
  assert.match(yml, /^language: \{standard: 20\}$/m);
  const parsed = KB.project.parseYaml(yml);
  assert.deepEqual(plain(parsed.language), { standard: 20 });
  assert.equal(KB.project.toSelection(fixture, parsed, null).selection.cxx, 20);
  const none = KB.generate.generate(fixture, image(), KB.shared.run(fixture, image())).files.find((f) => f.id === 'image').text;
  assert.ok(!/^language:/m.test(none));
});

test('the shared resolver sees the catalogue language and refuses a part above the standard', () => {
  const cat = plain(fixture);
  const bmp = cat.drivers.find((d) => d.name === 'bmp581');
  const v = bmp.versions.find((x) => x.version === '0.1.0');
  v.language = { standard: 23, profile: 'KB-CPP23', compilers: { gcc: '13' }, tested: [23], source: 'driver.yml' };
  const kbCat = KB.shared.kbCatalog(cat);
  assert.equal(kbCat.drivers.find((d) => d.name === 'bmp581').versions.find((x) => x.version === '0.1.0').language.standard, 23);
  const r = KB.shared.run(cat, image({ cxx: 20 }));
  const err = r.shared.errors.find((e) => e.code === 'KB-RES-024');
  assert.ok(err, JSON.stringify(r.shared.errors));
  assert.equal(err.subject, 'bmp581');
  assert.ok(r.findings.some((f) => f.code === 'KB-RES-024' && f.severity === 'error'));
  assert.equal(r.ok, false);
  // Undeclared: no check, the same selection resolves.
  assert.ok(!KB.shared.run(cat, image()).shared.errors.some((e) => e.code === 'KB-RES-024'));
});

test('the shipped catalogue records a language for every driver and kb-core version', () => {
  const site = ctx.KB_CATALOG;
  for (const d of site.drivers) {
    for (const v of d.versions) {
      assert.ok(v.language && [17, 20, 23, 26].includes(v.language.standard), `${d.name} ${v.version}`);
      assert.ok(['driver.yml', 'default'].includes(v.language.source), `${d.name} ${v.version}`);
    }
  }
  for (const r of site.kb_core.releases) {
    assert.ok(r.language && ['kb.yml', 'default'].includes(r.language.source), r.version);
  }
});
