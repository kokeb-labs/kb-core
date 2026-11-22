// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Step 5 (js/views.js): the part table follows the CMake dependency closure, the models read the evidence, and the
// size budgets of docs/design (visualization plan, section 4.3) hold for the views and their data.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { load, root } from './load.mjs';

const ctx = await load();
const { KB } = ctx;
const repo = join(root, '..');

test('the part table is the dependency closure of cmake/kb_build_settings.cmake', () => {
  const cmake = readFileSync(join(repo, 'cmake', 'kb_build_settings.cmake'), 'utf8');
  for (const p of KB.views.model.PARTS) {
    const m = new RegExp(`set\\(_KB_DEPS_${p.id}([^)]*)\\)`).exec(cmake);
    assert.ok(m, p.id);
    const deps = m[1].replace(/"/g, '').trim().split(/\s+/).filter(Boolean).sort();
    assert.deepEqual([...p.needs].sort(), deps, p.id);
  }
});

test('the architecture model carries the evidence of the selected kb-core version', () => {
  const site = ctx.KB_CATALOG;
  const version = site.kb_core.releases[0].version;
  const image = { kb_core: version, port: null, mcu: { family: 'host', target: 'host' }, rtos: 'none', drivers: [], buses: {} };
  const a = KB.views.model.architecture(site, image, null);
  const ev = KB.views.model.evidenceFor(version);
  if (ev) {
    assert.equal(a.evidence.release.version, version);
    const utils = a.parts.find((p) => p.id === 'utils');
    assert.equal(utils.tests, ev.tests.parts.find((p) => p.part === 'utils').tests);
  }
  assert.equal(a.parts.length, 5);
  const rows = KB.views.model.versions(site);
  assert.equal(rows.length, site.kb_core.releases.length);
});

test('every evidence document matches kb-evidence/1 at its top level and is the version it is keyed by', () => {
  for (const [version, ev] of Object.entries(ctx.KB_EVIDENCE || {})) {
    assert.equal(ev.schema, 'kb-evidence/1');
    assert.equal(ev.release.version, version);
    assert.ok(ev.protocol.length > 0 && ev.protocol.every((p) => ['pass', 'fail'].includes(p.status)));
    assert.equal(ev.passed, ev.protocol.every((p) => p.status === 'pass'));
  }
});

test('size budgets of the page: first-view scripts 120 KB, stylesheet 25 KB (visualization plan 4.3, never raised)', () => {
  const html = readFileSync(join(root, 'index.html'), 'utf8');
  const own = [...html.matchAll(/<script src="(js\/[a-z]+\.js)"/g)].map((m) => m[1]);
  // The first view is the shell: the page, its picker and the resolver and generator it runs. The explorer (views.js),
  // "Open an existing project" (project.js, upgrade.js), the supply-chain panel (panel.js, deferred), the vendored
  // library, the data and resolver.mjs load beside it
  // but are budgeted on their own.
  const later = ['js/views.js', 'js/project.js', 'js/upgrade.js', 'js/panel.js'];
  const first = own.filter((f) => !later.includes(f));
  assert.ok(first.includes('js/ui.js') && first.includes('js/picker.js'), first.join());
  const bytes = first.reduce((a, f) => a + statSync(join(root, f)).size, 0);
  assert.ok(bytes <= 120000, `first-view script ${bytes} bytes over ${first.join(', ')}`);
  for (const f of later) assert.ok(statSync(join(root, f)).size <= 40000, f);
  assert.ok(statSync(join(root, 'css/app.css')).size <= 25000, 'css/app.css');
});

test('size budgets: each view module 40 KB, evidence 150 KB per release, offline.html 1.5 MB', () => {
  const kb = (f) => statSync(join(root, f)).size / 1024;
  assert.ok(kb('js/views.js') <= 40, `js/views.js ${kb('js/views.js').toFixed(1)} KB`);
  const releases = Object.keys(ctx.KB_EVIDENCE || {}).length || 1;
  assert.ok(kb('data/evidence.js') / releases <= 150, 'data/evidence.js');
  assert.ok(kb('offline.html') <= 1536, `offline.html ${kb('offline.html').toFixed(0)} KB`);
});
