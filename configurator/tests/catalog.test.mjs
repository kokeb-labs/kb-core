// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { load, root } from './load.mjs';

const py = (args) => spawnSync('python3', args, { cwd: root, encoding: 'utf8' });
const catalog = JSON.parse(readFileSync(join(root, 'data', 'catalog.json'), 'utf8'));

test('catalog.json, the fixture and every vector match their schemas', () => {
  const vectors = readdirSync(join(root, 'tests', 'vectors', 'resolver')).map((f) => join('tests', 'vectors', 'resolver', f));
  for (const [schema, targets] of [
    ['catalog.schema.json', ['data/catalog.json', 'tests/vectors/catalog-fixture.json']],
    ['resolver-vector.schema.json', vectors],
    ['kb-image.schema.json', vectors.map((v) => v + '#image')],
  ]) {
    const r = py(['tools/validate.py', join('schemas-draft', schema), ...targets]);
    assert.equal(r.status, 0, r.stdout + r.stderr);
  }
});

test('catalog.js carries exactly catalog.json', async () => {
  const ctx = await load();
  assert.deepEqual(JSON.parse(JSON.stringify(ctx.KB_CATALOG)), catalog);
});

test('the catalogue is sorted, pins tags only and every figure exists', () => {
  const names = catalog.drivers.map((d) => d.name);
  assert.deepEqual(names, [...names].sort());
  for (const d of catalog.drivers) {
    for (const v of d.versions) {
      assert.match(v.tag, /^v\d+\.\d+\.\d+(-rc\.\d+)?$/);
      assert.equal(v.kind, v.tag.includes('-rc.') ? 'rc' : 'release');
    }
    for (const f of d.figures) {
      assert.ok(existsSync(join(root, f.light)), f.light);
      assert.ok(existsSync(join(root, f.dark)), f.dark);
      const svg = readFileSync(join(root, f.light), 'utf8');
      assert.ok(!/<script|\son[a-z]+=|href="https?:/i.test(svg), `${f.light} is a plain drawing`);
    }
    assert.equal(d.status, d.versions.some((v) => v.kind === 'release') ? 'release' : d.versions.length ? 'rc' : 'unpublished');
  }
});

test('the catalogue builder parses driver metadata the way the drivers write it', () => {
  const code = `
import sys; sys.path.insert(0, 'tools')
import build_catalog as b
m = b.parse_flat_yaml("---\\nname: x\\nbus: [i2c, spi]   # c\\nmcus: [any]\\nperipheral: null\\nkb_core_min: null\\nauthors: [\\"The x authors\\"]\\n")
assert m == {"name": "x", "bus": ["i2c", "spi"], "mcus": ["any"], "peripheral": None, "kb_core_min": None, "authors": ["The x authors"]}, m
s, caps, addr = b.readme_facts("# X Driver\\n\\nC++20 driver for the X, over [I2C](a).\\n\\n## At a glance\\n\\n| | |\\n| --- | --- |\\n| **Bus** | I2C at \`0x46\` or \`0x47\`, or SPI |\\n| **Driver** | header only |\\n| **Measures** | **pressure** |\\n")
assert s == "C++20 driver for the X, over I2C.", s
assert caps == [{"label": "Bus", "text": "I2C at 0x46 or 0x47, or SPI"}, {"label": "Measures", "text": "pressure"}], caps
assert addr == [0x46, 0x47], addr
_, _, rng = b.readme_facts("# Y\\n\\n## At a glance\\n\\n| **Bus** | I2C, 7-bit address \`0x20\` to \`0x27\` |\\n")
assert rng == list(range(0x20, 0x28)), rng
assert b.tag_key("v0.1.0") > b.tag_key("v0.1.0-rc.3") > b.tag_key("v0.1.0-rc.1")
assert b.licence_facts("MIT License\\n\\nCopyright (c) 2026 The y authors\\n") == {"spdx": "MIT", "copyright": "Copyright (c) 2026 The y authors"}
print("ok")
`;
  const r = spawnSync('python3', ['-c', code], { cwd: root, encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
});
