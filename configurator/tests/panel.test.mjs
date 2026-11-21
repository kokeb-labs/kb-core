// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// js/panel.js, the supply-chain panel: which components a selection pulls in, how the vulnerability data reads (none
// known, not scanned, missing, stale from the 11th day), the licence summary, the attestation command, the evidence
// chips and where a blocker's cause is. The model is pure; render() is exercised by tools/headless_check.mjs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { load } from './load.mjs';

const ctx = await load();
const { KB } = ctx;
const M = KB.panel.model;
const site = ctx.KB_CATALOG;
const plain = (x) => JSON.parse(JSON.stringify(x));
const newest = site.kb_core.releases.find((r) => r.kind !== 'dev') || site.kb_core.releases[0];

function run(drivers, family = 'esp32', target = 'esp32s3', rtos = 'freertos') {
  const hash = `#v=1&f=${family}&t=${target}&b=&r=${rtos}&k=${newest.version}&p=&n=t&d=${drivers.map((d) => `${d}~0.1.0-rc.1~${d === 'ws2812_rmt' ? 'pio' : 'i2c'}~${d === 'ws2812_rmt' ? 'pio0' : 'i2c0'}`).join(',')}&m=${drivers.map((d) => (d === 'ws2812_rmt' ? 'pio0~o' : 'i2c0~g')).join(',')}`;
  const image = KB.state.decode(hash);
  const result = KB.shared.run(site, image);
  return { catalog: site, image, result, output: KB.generate.generate(site, image, result) };
}
const day = (s) => new Date(`${s}T12:00:00Z`);
const fx = (checked, extra = []) => ({ schema: 'kb-vulns/1', checked_at: checked, stale_after_days: 10, components: [
  { name: 'driver-bmp581', version: 'v0.1.0-rc.1', status: 'none known', count: 0, advisories: [] },
  { name: 'esp-idf', version: 'v5.5', status: 'advisories found', count: 1,
    advisories: [{ id: 'GHSA-aaaa', severity: 'high', summary: 'x', url: 'https://osv.dev/vulnerability/GHSA-aaaa' },
      { id: 'GHSA-bad', severity: 'low', summary: 'y', url: 'javascript:alert(1)' }] }, ...extra] });

test('the selected components follow the selection: kb-core, port, drivers, SDK and RTOS', () => {
  const a = M.components(run(['bmp581']).output).map((c) => c.name);
  assert.ok(a.includes('kb-core') && a.includes('driver-bmp581') && a.includes('esp-idf') && a.includes('freertos-kernel'), a.join());
  assert.ok(!a.includes('driver-ws2812-rmt'));
  const b = M.components(run(['bmp581', 'ws2812_rmt']).output).map((c) => c.name);
  assert.equal(b.length, a.length + 1);
  assert.ok(b.includes('driver-ws2812-rmt'));
  const host = M.components(run([], 'host', 'host', 'none').output).map((c) => c.name);
  assert.ok(!host.includes('esp-idf'), host.join());
});

test('stale from the 11th day: 10 days old is fresh, 11 is stale, a future date is fresh', () => {
  const d = fx('2026-11-01');
  assert.equal(M.freshness(d, day('2026-11-01')).state, 'fresh');
  assert.equal(M.freshness(d, day('2026-11-11')).state, 'fresh');
  assert.equal(M.freshness(d, day('2026-11-11')).age, 10);
  assert.equal(M.freshness(d, day('2026-11-12')).state, 'stale');
  assert.equal(M.freshness(d, day('2026-10-30')).state, 'fresh');
  // late in the evening of day 10 in UTC is still day 10
  assert.equal(M.freshness(d, new Date('2026-11-11T23:59:59Z')).state, 'fresh');
});

test('none known only with data; not scanned for a component the data lacks; missing data says so', () => {
  const comps = M.components(run(['bmp581']).output);
  const ok = M.cve(fx('2026-11-20', []), comps.filter((c) => c.name === 'driver-bmp581'), day('2026-11-21'));
  assert.equal(ok.status, 'none known');
  assert.equal(ok.items[0].state, 'none known');
  const mixed = M.cve(fx('2026-11-20'), comps, day('2026-11-21'));
  const by = Object.fromEntries(mixed.items.map((i) => [i.name, i.state]));
  assert.equal(by['driver-bmp581'], 'none known');
  assert.equal(by['esp-idf'], 'advisories');
  assert.equal(by['kb-core'], 'not scanned');
  assert.equal(by['freertos-kernel'], 'not scanned');
  assert.equal(mixed.status, 'advisories found');
  assert.equal(mixed.advisories, 2);
  assert.equal(mixed.notScanned, mixed.items.filter((i) => i.state === 'not scanned').length);
  const none = M.cve(fx('2026-11-20'), comps.filter((c) => c.name === 'kb-core'), day('2026-11-21'));
  assert.equal(none.status, 'not scanned');
  for (const bad of [null, undefined, {}, { checked_at: 'soon', components: [] }, { checked_at: '2026-11-20' }]) {
    const miss = M.cve(bad, comps, day('2026-11-21'));
    assert.equal(miss.status, 'no data');
    assert.ok(miss.missing && miss.items.every((i) => i.state === 'not scanned'));
  }
  assert.equal(M.cve(fx('2026-10-01'), comps, day('2026-11-21')).stale, true);
});

test('a pin at another version than the scan is not scanned; the SDK matches by name; links must be https', () => {
  const other = M.cve(fx('2026-11-20'), [{ name: 'driver-bmp581', label: 'BMP581', version: '0.2.0' }, { name: 'esp-idf', label: 'esp-idf', version: 'v5.5' },
    { name: 'threadx', label: 'threadx', version: null }], day('2026-11-21'));
  assert.equal(other.items[0].state, 'not scanned');
  const sdk = other.items[1];
  assert.deepEqual(plain(sdk.advisories.map((a) => a.url)), ['https://osv.dev/vulnerability/GHSA-aaaa', null]);
  const real = M.cve(ctx.KB_VULNERABILITIES, M.components(run(['bmp581']).output), day(ctx.KB_VULNERABILITIES.checked_at));
  assert.equal(real.items.find((i) => i.name === 'driver-bmp581').state, 'none known');
});

test('licence summary: all MIT, or the sorted list', () => {
  assert.equal(M.licences([{ licence: 'MIT' }, { licence: 'MIT' }]).text, 'all MIT');
  assert.equal(M.licences([{ licence: 'MIT' }, { licence: 'Apache-2.0' }, { licence: 'MIT' }]).text, 'Apache-2.0, MIT');
  assert.equal(M.licences([]).text, 'none');
  const r = run(['bmp581']);
  const b = M.sbom(r.output, KB.generate.lockComponents);
  assert.ok(b.ready && b.count >= 6 && b.pinned >= 1 && b.pins >= b.pinned);
  const broken = run(['bmp581'], 'stm32', 'stm32f4', 'freertos');
  assert.equal(M.sbom(broken.output, KB.generate.lockComponents).ready, false);
});

test('provenance: the release asset and the verify command, "per release process", never invented', () => {
  const ev = { release: { version: '1.2.3' }, supply: { release_assets: [{ name: 'kb-core-1.2.3.spdx.json', sha256: 'ab' }, { name: 'kb-core-1.2.3.tar.gz', sha256: 'cd' }] } };
  const p = M.provenance(ev, 'kokeb-labs/kb-core');
  assert.equal(p.state, 'per release process');
  assert.equal(p.command, 'gh attestation verify kb-core-1.2.3.tar.gz --repo kokeb-labs/kb-core');
  assert.equal(M.provenance(null, 'kokeb-labs/kb-core').state, 'none');
  assert.equal(M.provenance({ release: { version: '1' }, supply: { release_assets: [] } }, 'a/b').state, 'none');
  const evil = { release: { version: '1' }, supply: { release_assets: [{ name: 'x; rm -rf ~', sha256: '' }] } };
  assert.equal(M.provenance(evil, 'a/b').command, null);
  assert.equal(M.provenance(ev, 'a/b; id').command, null);
});

test('safety chips: kb-core evidence labelled as such, a driver block when the data has one', () => {
  const version = Object.keys(ctx.KB_EVIDENCE)[0];
  const ev = ctx.KB_EVIDENCE[version];
  const image = { drivers: [{ name: 'bmp581' }, { name: 'bno08x' }] };
  const rows = M.safety(ev, image);
  assert.equal(rows.length, 2);
  for (const r of rows) {
    assert.equal(r.label, 'kb-core evidence');
    assert.equal(r.own, false);
    assert.deepEqual(plain(r.chips.map((c) => c.id)), ['tests', 'coverage', 'misra']);
  }
  assert.equal(rows[0].chips[0].text, `Tests ${ev.tests.total}`);
  assert.match(rows[0].chips[0].href, /^https:\/\/github\.com\//);
  const own = { tests: { total: 5, failures: 0 }, coverage: { line: { percent: 90 }, branch: { percent: 80 }, floors: { branch: 75 } }, misra: { passed: false, edition: 'x' },
    source: { run_url: 'https://example.org/run/1' } };
  const mixed = M.safety({ ...ev, drivers: { bmp581: own } }, image);
  assert.equal(mixed[0].label, 'driver evidence');
  assert.equal(mixed[0].chips[0].text, 'Tests 5');
  assert.equal(mixed[0].chips[0].href, 'https://example.org/run/1');
  assert.equal(mixed[0].chips[2].tone, 'err');
  assert.equal(mixed[1].label, 'kb-core evidence');
  assert.deepEqual(plain(M.safety(null, image)[0].chips), []);
  assert.deepEqual(plain(M.chips({ tests: { total: 'x' } }, {})), []);
});

test('blockers are the errors; warnings and notes go to details; the cause is found from the subject', () => {
  const f = [{ severity: 'warning', code: 'W' }, { severity: 'error', code: 'E', subject: 'buses.i2c0' }, { severity: 'info', code: 'I' }];
  assert.deepEqual(plain(M.blockers(f).map((x) => x.code)), ['E']);
  assert.deepEqual(plain(M.details(f).map((x) => x.code)), ['W', 'I']);
  const image = { drivers: [{ name: 'bno08x', bus: 'i2c0' }, { name: 'pca9685', bus: 'i2c1' }, { name: 'fdo2', bus: 'uart1' }] };
  assert.deepEqual(plain(M.causeOf('bno08x', image)), { kind: 'driver', id: 'bno08x' });
  assert.deepEqual(plain(M.causeOf('buses.i2c0.model', image)), { kind: 'bus', id: 'i2c0' });
  assert.deepEqual(plain(M.causeOf('i2c0/bno08x', image)), { kind: 'bus', id: 'i2c0' });
  assert.deepEqual(plain(M.causeOf('rtos', image)), { kind: 'rtos', id: null });
  assert.deepEqual(plain(M.causeOf('mcu.target', image)), { kind: 'target', id: null });
  assert.deepEqual(plain(M.causeOf('kb_core.version', image)), { kind: 'version', id: 'kb-version' });
  assert.equal(M.causeOf('tested', image), null);
  const bad = M.blockerDrivers([{ severity: 'error', subject: 'buses.i2c0' }, { severity: 'error', subject: 'fdo2' }, { severity: 'warning', subject: 'pca9685' }], image);
  assert.deepEqual([...bad].sort(), ['bno08x', 'fdo2']);
});

test('the summary line reads the selection: counts, licences, vulnerabilities and blockers', () => {
  const r = run(['bmp581']);
  const m = M.summaryOf(r, day(ctx.KB_VULNERABILITIES.checked_at), KB.generate.lockComponents);
  assert.match(m.summary, /\d+ components, .*; no known vulnerabilities in \d+ of \d+ scanned; no blockers\.$/);
  const old = M.summaryOf(r, day('2099-01-01'), KB.generate.lockComponents);
  assert.match(old.summary, /vulnerability data \d+ days old/);
  assert.equal(m.blockers.length, 0);
});
