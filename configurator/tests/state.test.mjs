// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { load } from './load.mjs';

const { KB } = await load();
const image = {
  image: 'weather-node', kb_core: null, port: null, rtos: 'freertos',
  mcu: { family: 'esp32', target: 'esp32s3', board: 'esp32s3-devkitc' },
  drivers: [{ name: 'bmp581', version: '0.1.0-rc.1', via: 'i2c', bus: 'i2c0' },
    { name: 'ws2812_rmt', version: '0.1.0-rc.1', via: 'pio', bus: 'pio0' }],
  buses: { i2c0: { model: 'guarded' }, pio0: { model: 'owner' } },
};

test('the hash round-trips a selection', () => {
  const hash = KB.state.encode(image);
  assert.match(hash, /^#v=1&/);
  assert.deepEqual(JSON.parse(JSON.stringify(KB.state.decode(hash))), image);
});

test('the hash carries only catalogue identifiers', () => {
  const hash = KB.state.encode({ ...image, image: 'Jane Doe <jane@example.com>' });
  assert.ok(!/@|%40|jane%20/i.test(hash.replace(/~/g, '')), hash);
  const keys = [...new URLSearchParams(hash.slice(1)).keys()];
  assert.deepEqual(keys, ['v', 'f', 't', 'b', 'r', 'k', 'p', 'n', 'd', 'm']);
});

test('hostile or foreign hashes are refused or cleaned', () => {
  assert.equal(KB.state.decode('#foo=bar'), null);
  const d = KB.state.decode('#v=1&f=<script>&d=x~1~i2c~i2c0,~~~,ok~0.1.0~spi~spi0');
  assert.equal(d.mcu.family, '');
  assert.deepEqual(JSON.parse(JSON.stringify(d.drivers.map((x) => x.name))), ['x', 'ok']);
});
