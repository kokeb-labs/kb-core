// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The driver picker's logic (js/picker.js): filter, sort, group, chunks, the view in the address bar, and the cascade.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { load } from './load.mjs';

const ctx = await load();
const { KB, KB_CATALOG: catalog } = ctx;
const P = KB.picker;
const kinds = catalog.bus_kinds;
const plain = (v) => JSON.parse(JSON.stringify(v));
const eq = (a, b, m) => assert.deepStrictEqual(plain(a), plain(b), m);
const names = (l) => l.map((d) => d.name);
const view = (o) => P.sanitize({ ...P.defaults(), ...o });
const image = (family, o = {}) => ({ image: 'x', kb_core: null, port: null, rtos: 'freertos', cxx: null, drivers: [], buses: {},
  mcu: { family, target: catalog.families.find((f) => f.id === family).targets[0].id, board: null }, ...o });

test('no filter lists every driver; a search runs over name, summary, tags, category and bus', () => {
  assert.equal(P.filter(catalog.drivers, view({}), null, kinds).items.length, catalog.drivers.length);
  const hit = (q) => names(P.filter(catalog.drivers, view({ q }), null, kinds).items);
  assert.ok(hit('bmp581').includes('bmp581'));
  assert.ok(hit('pressure').includes('bmp581'));
  const tagged = catalog.drivers.find((d) => (d.tags || []).includes('imu'));
  assert.ok(hit('imu').includes(tagged.name));
  assert.ok(hit('secure-element').length >= 1);
  eq(hit('zz-no-such-chip'), []);
  eq(hit('bmp581 imu'), []); // every word must match
});

test('chips: any chip of one group matches, groups combine', () => {
  const f = (o) => names(P.filter(catalog.drivers, view(o), null, kinds).items);
  const i2c = f({ bus: ['i2c'] });
  const spi = f({ bus: ['spi'] });
  assert.ok(i2c.length && spi.length);
  assert.equal(new Set(f({ bus: ['i2c', 'spi'] })).size, new Set([...i2c, ...spi]).size);
  const motor = f({ cat: ['motor'] });
  assert.ok(motor.every((n) => catalog.drivers.find((d) => d.name === n).category === 'motor'));
  const both = f({ cat: ['motor'], bus: ['spi'] });
  eq(both, motor.filter((n) => spi.includes(n)));
  eq(f({ tag: ['adc'] }), names(catalog.drivers.filter((d) => d.tags.includes('adc'))));
});

test('tag counts in the catalogue match the drivers', () => {
  for (const [tag, n] of Object.entries(catalog.tag_index.tags)) {
    assert.equal(catalog.drivers.filter((d) => d.tags.includes(tag)).length, n, tag);
  }
});

test('unavailable drivers leave the list and come back apart; "works with my target" off keeps them in', () => {
  const un = new Map([['bmp581', { reason: 'x' }]]);
  const on = P.filter(catalog.drivers, view({}), un, kinds);
  assert.ok(!names(on.items).includes('bmp581'));
  eq(names(on.unavailable), ['bmp581']);
  const off = P.filter(catalog.drivers, view({ fit: false }), un, kinds);
  assert.ok(names(off.items).includes('bmp581'));
  eq(off.unavailable, []);
  const narrowed = P.filter(catalog.drivers, view({ q: 'pf1550' }), un, kinds);
  eq(narrowed.unavailable, []); // the other filters still apply to the unavailable list
});

test('sort by name, category and bus, either way, with a stable tie break', () => {
  const by = (k, desc) => P.sort(catalog.drivers, k, desc, kinds);
  const n = names(by('name'));
  eq(n, [...n].sort());
  eq(names(by('name', true)), n.slice().reverse());
  const cats = by('category').map((d) => d.category);
  eq(cats, [...cats].sort());
  const buses = by('bus').map((d) => P.busesOf(d).map((b) => P.busLabel(kinds, b)).join(' ').toLowerCase());
  eq(buses, [...buses].sort());
  eq(names(by('bogus')), n);
});

test('grouping by category or bus partitions the list; sorting orders inside groups', () => {
  const sorted = P.sort(catalog.drivers, 'name', true, kinds);
  for (const by of ['category', 'bus']) {
    const groups = P.group(sorted, by, kinds);
    assert.equal(groups.reduce((a, x) => a + x.items.length, 0), catalog.drivers.length);
    eq(groups.map((x) => x.label), groups.map((x) => x.label).sort());
    for (const x of groups) eq(names(x.items), names(sorted).filter((n) => names(x.items).includes(n)));
  }
  assert.ok(P.group(sorted, 'category', kinds).every((x) => x.items.every((d) => d.category === x.key)));
});

test('filter, sort and group together', () => {
  const v = view({ cat: ['motor'], sort: 'bus', desc: true, group: 'bus', view: 'grouped' });
  const items = P.sort(P.filter(catalog.drivers, v, null, kinds).items, v.sort, v.desc, kinds);
  const groups = P.group(items, v.group, kinds);
  assert.ok(groups.every((x) => x.items.every((d) => d.category === 'motor')));
  assert.equal(groups.reduce((a, x) => a + x.items.length, 0), items.length);
});

test('a long result is shown in chunks of 24 past 60 items, a short one whole', () => {
  assert.equal(P.limit(22, 24), 22);
  assert.equal(P.limit(60, 24), 60);
  assert.equal(P.limit(61, 24), 24);
  assert.equal(P.limit(61, 48), 48);
  assert.equal(P.limit(61, 96), 61);
  const groups = [{ label: 'a', items: [1, 2, 3] }, { label: 'b', items: [4, 5] }, { label: 'c', items: [6] }];
  eq((P.take(groups, 4)), [{ label: 'a', items: [1, 2, 3] }, { label: 'b', items: [4] }]);
  eq(P.take(groups, 0), []);
});

test('sanitize refuses unknown values and hostile text', () => {
  const v = P.sanitize({ q: '<img src=x onerror=1>', bus: ['i2c', '<b>', 'I2C'], view: 'cube', sort: 'x', group: 'vendor', fit: 'no' });
  assert.equal(v.q, 'img srcx onerror1');
  eq((v.bus), ['i2c']);
  assert.equal(v.view, 'grid');
  assert.equal(v.sort, 'name');
  assert.equal(v.group, 'category');
  assert.equal(v.fit, true);
  assert.equal(P.sanitize({ q: 'a'.repeat(99) }).q.length, 40);
});

test('reset clears filters and order but keeps the view', () => {
  const v = P.reset(view({ q: 'a', bus: ['i2c'], sort: 'bus', desc: true, view: 'list', group: 'bus', fit: false }));
  assert.equal(P.filtered(v), false);
  assert.equal(v.sort, 'name');
  assert.equal(v.view, 'list');
  assert.equal(v.group, 'bus');
});

// -- the view in the address bar ------------------------------------------------------------------------------------
test('the view round-trips through the hash, and defaults add nothing', () => {
  const img = image('esp32');
  assert.equal(KB.state.encode(img, { view: P.defaults() }), KB.state.encode(img));
  assert.equal(KB.state.viewState(KB.state.encode(img)), null);
  const v = view({ q: 'pressure', bus: ['i2c', 'spi'], cat: ['sensor'], tag: ['imu'], fit: false, view: 'grouped', group: 'bus',
    sort: 'category', desc: true, unavail: true });
  const hash = KB.state.encode(img, { view: v });
  assert.match(hash, /&w=1&q=pressure&fb=i2c,spi&fc=sensor&ft=imu&fw=0&vw=g&gb=b&so=-c&su=1$/);
  eq((KB.state.viewState(hash)), plain(v));
  for (const mode of ['grid', 'list', 'grouped']) {
    const w = view({ view: mode, sort: 'bus' });
    assert.equal(KB.state.viewState(KB.state.encode(img, { view: w })).view, mode);
  }
});

test('older links and hostile view parameters stay safe', () => {
  const old = '#v=1&f=esp32&t=esp32s3&b=esp32s3-devkitc&r=freertos&k=&p=&n=weather-node&d=&m=';
  assert.equal(KB.state.viewState(old), null);
  assert.equal(KB.state.decode(old).mcu.target, 'esp32s3');
  assert.equal(KB.state.viewState(old + '&q=pressure'), null); // read only with w=1
  const bad = KB.state.viewState(old + '&w=1&q=%3Cscript%3E&fb=%3Cx%3E,i2c&vw=z&so=-zz');
  assert.equal(bad.q, 'script');
  eq((bad.bus), ['i2c']);
  assert.equal(bad.view, 'grid');
  assert.equal(bad.sort, 'name');
  assert.equal(bad.desc, true);
  // the selection decodes the same with or without the view
  eq((KB.state.decode(old + '&w=1&vw=l')), plain(KB.state.decode(old)));
});

// -- the cascade ----------------------------------------------------------------------------------------------------
test('the page asks availability only through the engine, with the image\'s choices', () => {
  const img = image('esp32', { cxx: 23, kb_core: '0.1.0-rc.1' });
  eq((P.selectionOf(img)), { family: 'esp32', target: 'esp32', rtos: 'freertos', language: { standard: 23 }, kb_core: '0.1.0-rc.1' });
  assert.equal(P.selectionOf(image('host', { rtos: 'host' })).language, undefined);
  const noEngine = P.unavailableMap(catalog, img);
  assert.equal(noEngine.size, 0);
});

test('a change drops what no longer fits, keeps the rest, and says why; undo restores it', () => {
  KB.engine = ctx.KB.engine;
  const bmp = { name: 'bmp581', version: '0.1.0-rc.1', via: 'i2c', bus: 'i2c0' };
  const ws = { name: 'ws2812_rmt', version: '0.1.0-rc.1', via: 'pio', bus: 'pio0' };
  const before = image('esp32', { drivers: [bmp, ws] });
  const after = P.clone(before);
  after.mcu = { family: 'host', target: catalog.families.find((f) => f.id === 'host').targets[0].id, board: null };
  after.rtos = 'host';
  const un = P.unavailableMap(catalog, after);
  const drops = P.cascade(before, after, un, catalog);
  eq(names(drops.kept), ['bmp581']);
  eq(drops.drivers.map((d) => d.name), ['ws2812_rmt']);
  assert.match(drops.drivers[0].reason, /MCU family/);
  assert.equal(drops.rtos.to, 'host');
  assert.match(drops.rtos.why, /FreeRTOS is not available on/);
  const line = P.describe(drops);
  assert.match(line, /ws2812_rmt removed: needs MCU family esp32/);
  // the undo snapshot is a deep copy: the earlier selection comes back whole
  eq((before.drivers), [bmp, ws]);
  assert.notEqual(P.clone(before).drivers, before.drivers);
  assert.equal(P.describe(P.cascade(before, P.clone(before), new Map(), catalog)), '');
});

test('a lower standard makes drivers unavailable; the family default is where it starts', () => {
  const img = image('esp32', { cxx: 17 });
  const un = P.unavailableMap(catalog, img);
  assert.equal(un.size, catalog.drivers.length);
  assert.equal(P.defaultStandard(catalog, 'esp32'), 20);
  eq(P.rtosFor(catalog, 'stm32').map((r) => r.id), ['freertos', 'threadx', 'none']);
  eq(P.rtosFor(catalog, 'esp32').map((r) => r.id), ['freertos']);
});

test('drivers sharing a bus are named in the tray hint', () => {
  const sel = [{ name: 'a', via: 'i2c', bus: 'i2c0' }, { name: 'b', via: 'i2c', bus: 'i2c0' }, { name: 'c', via: 'spi', bus: 'spi0' }];
  eq((P.sharedBuses(sel, kinds)), [{ bus: 'i2c0', names: ['a', 'b'] }]);
});
