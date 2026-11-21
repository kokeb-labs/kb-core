// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { load } from './load.mjs';

const { KB } = await load();

test('sha256 matches the reference implementation', () => {
  for (const s of ['', 'abc', 'a'.repeat(55), 'a'.repeat(56), 'a'.repeat(64), 'a'.repeat(1000), 'üñïcødé ✓']) {
    assert.equal(KB.core.sha256(s), createHash('sha256').update(s, 'utf8').digest('hex'), JSON.stringify(s.slice(0, 10)));
  }
});

test('versions: a release sorts above its release candidates', () => {
  const c = KB.core.compareVersions;
  assert.ok(c('0.1.0', '0.1.0-rc.9') > 0);
  assert.ok(c('0.1.0-rc.2', '0.1.0-rc.10') < 0);
  assert.ok(c('v1.0.0', '0.9.9') > 0);
  assert.equal(c('0.4.0', 'v0.4.0'), 0);
  assert.throws(() => c('main', '0.1.0'));
});

test('ranges', () => {
  assert.ok(KB.core.satisfies('0.4.2', '>=0.4.0 <0.5.0'));
  assert.ok(!KB.core.satisfies('0.5.0', '>=0.4.0 <0.5.0'));
  assert.ok(KB.core.satisfies('0.5.0-rc.1', '>=0.5.0-rc.1 <0.6.0'));
  assert.ok(KB.core.satisfies('1.2.3', ''));
});

test('yaml scalars stay strings', () => {
  const q = KB.core.yamlScalar;
  assert.equal(q('pf1550'), 'pf1550');
  assert.equal(q('0.1.0-rc.1'), '0.1.0-rc.1');
  assert.equal(q('0d032fc8'), '"0d032fc8"');
  assert.equal(q('yes'), '"yes"');
  assert.equal(q('a: b'), '"a: b"');
  assert.equal(q(null), 'null');
  assert.equal(q(true), 'true');
});

test('safe image names', () => {
  assert.equal(KB.core.safeName('My Board #1!'), 'my-board-1');
  assert.equal(KB.core.safeName(''), 'kb-image');
  assert.equal(KB.core.safeName('../../etc'), 'etc');
});
