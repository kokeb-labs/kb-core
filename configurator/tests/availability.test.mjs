// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// availability() of the shared resolver on the committed catalogue. The shared vectors (tests/resolver/vectors, 033 and
// up) pin the answer for both resolvers; these tests cover the real data.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { AVAILABILITY_CODES, availability } from '../resolver.mjs';
import { root } from './load.mjs';

const site = JSON.parse(readFileSync(join(root, 'data', 'catalog.json'), 'utf8'));
const copy = (v) => JSON.parse(JSON.stringify(v));
const byName = (r) => Object.fromEntries(r.unavailable.map((u) => [u.name, u]));

test('the defaults of a family offer every driver', () => {
  const got = availability(site, { family: 'esp32' });
  assert.deepEqual(got.unavailable, []);
  assert.deepEqual(got.available, site.drivers.map((d) => d.name).sort());
});

test('every driver is in exactly one list', () => {
  for (const sel of [{ family: 'esp32' }, { family: 'host' }, { family: 'stm32' }, { family: 'esp32', rtos: 'none' }]) {
    const got = availability(site, sel);
    const listed = [...got.available, ...got.unavailable.map((u) => u.name)].sort();
    assert.deepEqual(listed, site.drivers.map((d) => d.name).sort(), JSON.stringify(sel));
  }
});

test('the ESP32-only driver is not offered on the host', () => {
  const got = byName(availability(site, { family: 'host' }));
  assert.equal(got.ws2812_rmt.code, 'KB-RES-013');
  assert.equal(Object.keys(got).length, 1);
});

test('a planned family has no port', () => {
  const got = availability(site, { family: 'stm32' });
  assert.deepEqual(got.available, []);
  assert.deepEqual([...new Set(got.unavailable.map((u) => u.code))], ['KB-RES-004']);
  assert.match(got.unavailable[0].reason, /no port/);
});

test('the language standard is judged against the drivers', () => {
  const got = availability(site, { family: 'esp32', language: { standard: 17 } });
  assert.deepEqual([...new Set(got.unavailable.map((u) => u.code))], ['KB-RES-024']);
  assert.equal(got.unavailable.length, site.drivers.length);
  assert.match(got.unavailable[0].reason, /needs C\+\+20, project is C\+\+17/);
});

test('a peripheral the target lacks', () => {
  const cat = copy(site);
  cat.families.find((f) => f.id === 'esp32').targets.find((t) => t.id === 'esp32c6').peripherals = [];
  const got = byName(availability(cat, { family: 'esp32', target: 'esp32c6' }));
  assert.equal(got.ws2812_rmt.code, 'KB-RES-031');
  assert.match(got.ws2812_rmt.reason, /RMT/);
});

test('a catalogue without requires reads the newest version', () => {
  const cat = copy(site);
  for (const d of cat.drivers) delete d.requires;
  assert.deepEqual(availability(cat, { family: 'host' }), availability(site, { family: 'host' }));
});

test('an unknown target blocks every driver', () => {
  const got = availability(site, { family: 'esp32', target: 'nope' });
  assert.deepEqual([...new Set(got.unavailable.map((u) => u.code))], ['KB-RES-005']);
});

test('every code returned is documented', () => {
  for (const sel of [{ family: 'host' }, { family: 'stm32' }, { family: 'esp32', language: { standard: 17 } }]) {
    for (const u of availability(site, sel).unavailable) assert.ok(u.code in AVAILABILITY_CODES, u.code);
  }
});
