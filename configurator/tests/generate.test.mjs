// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { load, root } from './load.mjs';

const ctx = await load();
const { KB } = ctx;
const fixture = JSON.parse(readFileSync(join(root, 'tests', 'vectors', 'catalog-fixture.json'), 'utf8'));
const image = () => ({
  image: 'demo', kb_core: '0.4.0', port: '0.4.0', rtos: 'freertos',
  mcu: { family: 'esp32', target: 'esp32s3', board: 'esp32s3-devkitc' },
  drivers: [{ name: 'pf1550', version: '0.1.0', via: 'i2c', bus: 'i2c0' },
    { name: 'bmp581', version: '0.1.0', via: 'i2c', bus: 'i2c0' },
    { name: 'ws2812_rmt', version: '0.1.0', via: 'pio', bus: 'pio0' }],
  buses: { i2c0: { model: 'guarded' }, pio0: { model: 'owner' } },
});
const run = (img, cat = fixture) => {
  const r = KB.shared.run(cat, img);
  return { r, out: KB.generate.generate(cat, img, r) };
};
const file = (out, id) => out.files.find((f) => f.id === id).text;

test('same selection, same bytes, whatever the order it was picked in', () => {
  const a = run(image()).out;
  assert.ok(a.ready);
  const reversed = image();
  reversed.drivers.reverse();
  const b = run(reversed).out;
  for (const f of a.files) assert.equal(file(b, f.id), f.text, f.path);
  assert.deepEqual(Buffer.from(KB.generate.zipProject(ctx.fflate, a)), Buffer.from(KB.generate.zipProject(ctx.fflate, b)));
});

test('the ZIP is the same in every time zone', () => {
  const script = `
    import { load } from ${JSON.stringify(join(root, 'tests', 'load.mjs'))};
    import { readFileSync } from 'node:fs';
    import { createHash } from 'node:crypto';
    const c = await load();
    const cat = JSON.parse(readFileSync(${JSON.stringify(join(root, 'tests', 'vectors', 'catalog-fixture.json'))}, 'utf8'));
    const img = ${JSON.stringify(image())};
    const r = c.KB.shared.run(cat, img);
    const z = c.KB.generate.zipProject(c.fflate, c.KB.generate.generate(cat, img, r));
    console.log(createHash('sha256').update(z).digest('hex'));`;
  const hashes = ['UTC', 'Pacific/Honolulu', 'Asia/Kathmandu'].map((tz) =>
    spawnSync(process.execPath, ['--input-type=module', '-e', script], { env: { ...process.env, TZ: tz }, encoding: 'utf8' }).stdout.trim());
  assert.equal(new Set(hashes).size, 1, hashes.join(' '));
  assert.match(hashes[0], /^[0-9a-f]{64}$/);
});

test('the lock, presets, kb_image.cmake and kb-fetch.sh are the shared resolver\'s bytes', () => {
  const { r, out } = run(image());
  const files = KB.engine.files(r.shared.lock);
  for (const [path, id] of [['kb-image.lock.yml', 'lock'], ['CMakePresets.json', 'presets'], ['kb_image.cmake', 'cmake'], ['kb-fetch.sh', 'fetch']]) {
    assert.equal(file(out, id), files[path], path);
  }
  const lock = r.shared.lock;
  assert.equal(lock.kb_core.tag, 'v0.4.0');
  assert.deepEqual(lock.drivers.map((d) => d.name), ['bmp581', 'pf1550', 'ws2812_rmt']);
  assert.equal(lock.ports[0].in_tree, false);
  assert.equal(lock.ports[0].tag, 'v0.4.0');
});

test('kb-image.yml is the shared image schema, with an explicit kind where the name does not give it', () => {
  const { out } = run(image());
  const img = file(out, 'image');
  assert.match(img, /^schema: 1$/m);
  assert.match(img, /^kb_core: \{version: 0\.4\.0\}$/m);
  assert.match(img, /^mcu: \{family: esp32, target: esp32s3, port: \{version: 0\.4\.0\}\}$/m);
  assert.match(img, /^  i2c0: \{model: guarded, devices: \[bmp581, pf1550\]\}$/m);
  const modbus = image();
  modbus.drivers = [{ name: 'alicat_basis2', version: '0.1.0', via: 'modbus', bus: 'uart1' }];
  modbus.buses = { uart1: { model: 'owner' } };
  const m = run(modbus);
  assert.equal(m.r.shared.ok, true, JSON.stringify(m.r.shared.errors));
  assert.match(file(m.out, 'image'), /^  uart1: \{model: owner, kind: modbus, devices: \[alicat_basis2\]\}$/m);
});

test('kb-fetch.sh pins tags under external/, checks commits, never follows a branch or recurses', () => {
  const { out } = run(image());
  const sh = file(out, 'fetch');
  assert.ok(sh.startsWith('#!/bin/sh\n'));
  assert.ok(!/--recursive|branch =|checkout (main|master)/.test(sh));
  assert.match(sh, /rev-parse "refs\/tags\/\$tag\^\{commit\}"/);
  assert.match(sh, /pin 'external\/drivers\/driver-pf1550' 'https:\/\/github\.com\/kokeb-labs-drivers\/driver-pf1550\.git' 'v0\.1\.0' '[0-9a-f]{40}'/);
  assert.match(sh, /pin 'external\/port-esp32' 'https:\/\/github\.com\/kokeb-labs-drivers\/port-esp32\.git' 'v0\.4\.0'/);
  for (const [cmd, args] of [['sh', ['-n']], ['shellcheck', ['-s', 'sh', '-']]]) {
    const res = spawnSync(cmd, args, { input: sh, encoding: 'utf8' });
    if (res.error) continue; // shellcheck not installed
    assert.equal(res.status, 0, res.stdout + res.stderr);
  }
});

test('a development revision of kb-core is pinned by commit, an in-tree port is not a submodule', () => {
  const img = { ...image(), kb_core: '0.6.0', port: null, mcu: { family: 'host', target: 'host', board: 'host' }, rtos: 'host',
    drivers: [{ name: 'bmp581', version: '0.1.0', via: 'i2c', bus: 'i2c0' }], buses: { i2c0: { model: 'guarded' } } };
  const { r, out } = run(img);
  assert.equal(r.shared.ok, true, JSON.stringify(r.shared.errors));
  const sh = file(out, 'fetch');
  assert.match(sh, /pin 'external\/kb-core' 'https:\/\/github\.com\/kokeb-labs\/kb-core\.git' '' '[0-9a-f]{40}'/);
  assert.ok(!/external\/port-/.test(sh));
  assert.match(file(out, 'cmake'), /KB_DRIVER_BMP581_DIR "\$\{CMAKE_CURRENT_LIST_DIR\}\/external\/drivers\/driver-bmp581"/);
});

test('nothing downloadable while the selection has errors', () => {
  const img = image();
  img.rtos = 'none';
  const { r, out } = run(img);
  assert.equal(r.ok, false);
  assert.equal(out.ready, false);
  assert.match(file(out, 'lock'), /written once the selection resolves/);
  assert.equal(out.project.length, 0);
});

test('CycloneDX 1.6 and SPDX 2.3: the identifiers of kb sbom (from the lock digest), plus SOUP', () => {
  const { r, out } = run(image());
  const lock = r.shared.lock;
  const digest = KB.core.sha256(KB.engine.canonicalJson(lock));
  const cdx = JSON.parse(file(out, 'cdx'));
  assert.equal(cdx.bomFormat, 'CycloneDX');
  assert.equal(cdx.specVersion, '1.6');
  assert.equal(cdx.serialNumber, `urn:uuid:${KB.core.uuid5('kb-lock:' + digest)}`);
  assert.equal(cdx.metadata.timestamp, '2026-11-13T00:00:00Z');
  const refs = cdx.components.map((c) => c['bom-ref']);
  assert.deepEqual(JSON.parse(JSON.stringify(refs)), ['kb-core', 'port-esp32', 'driver-bmp581', 'driver-pf1550', 'driver-ws2812_rmt', 'soup:esp-idf', 'soup:freertos-kernel']);
  assert.deepEqual(JSON.parse(JSON.stringify(cdx.components[0].components.map((c) => c.name))), ['kb-utils', 'kb-rtos', 'kb-interfaces', 'kb-handlers']);
  const pf = cdx.components.find((c) => c['bom-ref'] === 'driver-pf1550');
  assert.equal(pf.purl, 'pkg:github/kokeb-labs-drivers/driver-pf1550@v0.1.0');
  assert.deepEqual(pf.licenses, [{ license: { id: 'MIT' } }]);
  assert.ok(pf.properties.some((p) => p.name === 'kb:osv' && /^scanned 2026-11-13T00:00:00Z/.test(p.value)));
  const ws = cdx.components.find((c) => c['bom-ref'] === 'driver-ws2812_rmt');
  assert.ok(ws.properties.some((p) => p.name === 'kb:osv' && p.value === 'not scanned'));
  const spdx = JSON.parse(file(out, 'spdx'));
  assert.equal(spdx.spdxVersion, 'SPDX-2.3');
  assert.equal(spdx.dataLicense, 'CC0-1.0');
  assert.equal(spdx.documentNamespace, `https://kokeb-labs.github.io/kb-core/spdx/demo-${digest}`);
  const ids = new Set(spdx.packages.map((p) => p.SPDXID));
  assert.equal(ids.size, spdx.packages.length);
  for (const id of ids) assert.match(id, /^SPDXRef-[A-Za-z0-9.-]+$/);
  for (const rel of spdx.relationships) {
    assert.ok(rel.spdxElementId === 'SPDXRef-DOCUMENT' || ids.has(rel.spdxElementId));
    assert.ok(ids.has(rel.relatedSpdxElement));
  }
  const p = spdx.packages.find((x) => x.name === 'driver-pf1550');
  assert.equal(p.downloadLocation, 'git+https://github.com/kokeb-labs-drivers/driver-pf1550.git@v0.1.0');
  assert.equal(p.copyrightText, 'Copyright (c) The driver-pf1550 authors');
});

test('evidence reports: tested set, CVE status, licences', () => {
  const { out } = run(image());
  const status = file(out, 'status');
  assert.match(status, /\*\*Untested combination\.\*\*/);
  assert.match(status, /driver-ws2812-rmt \| 0\.1\.0 \| not scanned/);
  assert.match(status, /driver-pf1550 \| 0\.1\.0 \| 2026-11-13T00:00:00Z, osv\.dev 2026-11-13: no known vulnerabilities/);
  const tested = image();
  tested.drivers = tested.drivers.filter((d) => d.name !== 'ws2812_rmt');
  delete tested.buses.pio0;
  assert.match(file(run(tested).out, 'status'), /Tested: this combination is in kb 2026\.1/);
  const lic = file(out, 'licences');
  assert.match(lic, /\| driver-pf1550 \| 0\.1\.0 \| MIT \| Copyright \(c\) 2026 The driver-pf1550 authors \|/);
  assert.match(lic, /Licences in this image: Apache-2\.0, MIT\./);
});

test('the starter project unpacks with the expected tree and executable scripts', () => {
  const { out } = run(image());
  const zip = KB.generate.zipProject(ctx.fflate, out);
  const files = Object.keys(ctx.fflate.unzipSync(zip)).sort();
  assert.deepEqual(files, ['demo/.gitignore', 'demo/CMakeLists.txt', 'demo/CMakePresets.json', 'demo/README.md',
    'demo/components/kb/CMakeLists.txt', 'demo/components/kb_port_esp32/CMakeLists.txt', 'demo/evidence/licences.md', 'demo/evidence/sbom.cdx.json', 'demo/evidence/sbom.spdx.json', 'demo/evidence/status.md',
    'demo/kb-fetch.sh', 'demo/kb-image.lock.yml', 'demo/kb-image.yml', 'demo/kb_image.cmake',
    'demo/main/CMakeLists.txt', 'demo/main/board.cpp', 'demo/main/main.cpp']);
  const cm = out.project.find((f) => f.path === 'CMakeLists.txt').text;
  assert.match(cm, /include\(\$\{CMAKE_CURRENT_LIST_DIR\}\/kb_image\.cmake\)/);
  // ESP-IDF names a component after its folder: the forwarding folders give kb-core and the port their names, each
  // driver checkout is the component driver-<chip>, and main requires all of them (exports build them for real).
  assert.match(cm, /set\(EXTRA_COMPONENT_DIRS .*external\/drivers\/driver-pf1550/);
  assert.match(out.project.find((f) => f.path === 'components/kb/CMakeLists.txt').text, /external\/kb-core" ABSOLUTE\)/);
  assert.match(out.project.find((f) => f.path === 'main/CMakeLists.txt').text, /REQUIRES kb kb_port_esp32 driver-/);
  assert.match(out.project.find((f) => f.path === 'main/board.cpp').text, /#include "kb\/handlers\/bindings\/pf1550\.hpp"/);
  assert.match(file(out, 'cmake'), /set\(KB_DRIVER_PF1550_DIR "\$\{CMAKE_CURRENT_LIST_DIR\}\/external\/drivers\/driver-pf1550"\)/);
  const host = image();
  host.mcu = { family: 'host', target: 'host', board: 'host' };
  host.rtos = 'none';
  host.port = null;
  host.drivers = host.drivers.filter((d) => d.name !== 'ws2812_rmt');
  delete host.buses.pio0;
  const h = run(host).out;
  assert.ok(h.ready);
  const hcm = h.project.find((f) => f.path === 'CMakeLists.txt').text;
  assert.match(hcm, /target_link_libraries\(app PRIVATE board kb::utils kb::rtos\)/, 'applications never link interfaces or handlers');
  const buf = Buffer.from(zip);
  const modes = {};
  let i = buf.indexOf(Buffer.from([0x50, 0x4b, 0x01, 0x02]));
  while (i >= 0) {
    const n = buf.readUInt16LE(i + 28);
    modes[buf.subarray(i + 46, i + 46 + n).toString()] = buf.readUInt32LE(i + 38) >>> 16;
    i = buf.indexOf(Buffer.from([0x50, 0x4b, 0x01, 0x02]), i + 4);
  }
  assert.equal(modes['demo/kb-fetch.sh'], 0o100755);
  assert.equal(modes['demo/README.md'], 0o100644);
});
