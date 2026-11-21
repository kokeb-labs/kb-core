// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Step 5 of the page, "Inside and tested": three views of the kb-core version the selection uses.
//   Architecture  the parts, the port and the selected drivers as a layered map; Enter or a click opens a part's
//                 details (what it holds, what it needs, its tests, coverage, mutants, MISRA scope, docs links)
//   Evidence      what CI tested for that release and what it found (data/evidence.js, kb-evidence/1, built from the
//                 CI artifacts and release assets alone by tools/evidence/collect.py), every chart with its table
//   Versions      every kb-core version of the catalogue with its headline evidence, and the change from the one
//                 before
// No library, no network, no inline style: DOM calls, classes and SVG; the model functions are pure (KB.views.model)
// so node --test checks them.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const DOCS = 'https://kokeb-labs.github.io/kb-core/latest/';

  // kb-core's parts and what each needs: the dependency closure of cmake/kb_build_settings.cmake (_KB_DEPS_*),
  // which a test compares with this table.
  const PARTS = [
    { id: 'handlers', label: 'kb::handlers', layer: 0, needs: ['interfaces', 'rtos', 'utils'], tone: 'handler',
      holds: 'Driver bindings, the handler core, the start-up pipeline and the two thread models (owner task, guarded facade).',
      docs: 'parts/handlers/' },
    { id: 'interfaces', label: 'kb::interfaces', layer: 1, needs: ['rtos', 'utils'], tone: 'contract',
      holds: 'Bus and pin contracts as C++20 concepts (I2C, SPI, UART, GPIO, ADC, PWM, CAN, time, async) and the host test port.',
      docs: 'parts/interfaces/' },
    { id: 'rtos', label: 'kb::rtos', layer: 2, needs: ['utils'], tone: 'rtos',
      holds: 'Sleeps, locks, queues, threads and timers over FreeRTOS, ThreadX, none or std::thread.',
      docs: 'parts/rtos/' },
    { id: 'dma', label: 'kb::dma', layer: 2, needs: ['utils'], tone: 'contract',
      holds: 'DMA buffers, leases, cache-policy regions and copy-out streams.',
      docs: 'parts/dma/' },
    { id: 'utils', label: 'kb::utils', layer: 3, needs: [], tone: 'core',
      holds: 'The error model, time units, containers, CRCs and byte helpers that every part uses.',
      docs: 'parts/utils/' },
  ];

  const pct = (v) => (typeof v === 'number' ? `${v.toFixed(1)} %` : '-');

  // -- the model (pure) ---------------------------------------------------------------------------------------
  function evidenceFor(version) {
    const all = g.KB_EVIDENCE || {};
    return version && all[version] ? all[version] : null;
  }

  // The parts the selection builds, with their evidence; the port; the selected drivers.
  function architecture(catalog, image, lock) {
    const ev = evidenceFor(image.kb_core);
    const used = new Set(lock ? lock.parts : catalog.kb_core.parts);
    const by = (list, part) => (list || []).find((x) => x.part === part) || null;
    const parts = PARTS.map((p) => {
      const cov = ev ? by(ev.coverage.parts, p.id) : null;
      const mut = ev ? by(ev.mutation.parts, p.id) : null;
      const tests = ev ? by(ev.tests.parts, p.id) : null;
      return { ...p, used: used.has(p.id), neededBy: PARTS.filter((q) => q.needs.includes(p.id)).map((q) => q.id),
        tests: tests ? tests.tests : null, coverage: cov, mutation: mut };
    });
    const fam = catalog.families.find((f) => f.id === image.mcu.family);
    const port = fam && fam.port ? { id: 'port', label: `port ${fam.port.name}`, layer: 1,
      where: fam.port.releases && fam.port.releases.length ? fam.port.repo : `inside kb-core (${fam.port.path})`,
      version: image.port || (fam.port.releases && fam.port.releases.length ? null : image.kb_core) } : null;
    const drivers = (image.drivers || []).map((d) => {
      const c = catalog.drivers.find((x) => x.name === d.name);
      const v = c && c.versions.find((x) => x.version === d.version);
      return { id: 'driver-' + d.name, name: d.name, chip: c ? c.chip : d.name, version: d.version, bus: d.bus,
        model: image.buses && image.buses[d.bus] ? image.buses[d.bus].model : null,
        cxx: v && v.language ? v.language.standard : 20, repo: c ? c.repo : null };
    });
    return { evidence: ev, parts, port, drivers, rtos: image.rtos };
  }

  // Every kb-core version of the catalogue, newest first, with its headline evidence and the change from the
  // version before it (null when either has no evidence).
  function versions(catalog) {
    const rel = (catalog.kb_core.releases || []).slice()
      .sort((a, b) => KB.core.compareVersions(b.version, a.version));
    const head = (ev) => (ev ? { tests: ev.tests.total, line: ev.coverage.line.percent, branch: ev.coverage.branch.percent,
      mcdc: ev.coverage.condition.percent, mutation: ev.mutation.score, misra: ev.misra.findings.violations,
      deviated: ev.misra.findings.deviated, fuzz: ev.fuzz.planned_seconds, passed: ev.passed } : null);
    const rows = rel.map((r) => ({ version: r.version, kind: r.kind, date: r.date || null, commit: r.commit,
      bindings: (r.bindings || []).length, cxx: r.language ? r.language.standard : 20, evidence: head(evidenceFor(r.version)) }));
    rows.forEach((row, i) => {
      const prev = rows[i + 1];
      row.delta = row.evidence && prev && prev.evidence
        ? Object.fromEntries(['tests', 'line', 'branch', 'mcdc', 'mutation', 'misra'].map((k) => [k, +(row.evidence[k] - prev.evidence[k]).toFixed(1)]))
        : null;
    });
    return rows;
  }

  // -- rendering ---------------------------------------------------------------------------------------------
  const doc = g.document;
  function h(tag, attrs, ...children) {
    const el = doc.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'text') el.textContent = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? '' : String(v));
    }
    for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) el.append(c.nodeType ? c : doc.createTextNode(String(c)));
    return el;
  }
  const SVG = 'http://www.w3.org/2000/svg';
  function s(tag, attrs, ...children) {
    const el = doc.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs || {})) if (v !== null && v !== undefined) el.setAttribute(k, String(v));
    for (const c of children.flat()) if (c) el.append(c.nodeType ? c : doc.createTextNode(String(c)));
    return el;
  }
  const table = (caption, head, rows) => h('table', { class: 'data' }, h('caption', { text: caption }),
    h('thead', {}, h('tr', {}, head.map((x) => h('th', { scope: 'col', text: x })))),
    h('tbody', {}, rows.map((r) => h('tr', {}, r.map((c, i) => (i === 0 ? h('th', { scope: 'row' }, c) : h('td', {}, c)))))));
  const link = (href, text) => h('a', { href, target: '_blank', rel: 'noopener noreferrer', text });

  // A horizontal bar chart (0 to 100): one row per item, the value written at the end of its bar, a dashed floor
  // line when there is one. Its table twin is built from the same rows by the caller.
  function bars(title, rows, floor) {
    const W = 320; const L = 92; const RH = 24; const H = rows.length * RH + 28;
    const x = (v) => L + ((W - L - 44) * Math.max(0, Math.min(100, v))) / 100;
    const svg = s('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart', role: 'img', 'aria-label': title });
    svg.append(s('title', {}, title));
    rows.forEach((r, i) => {
      const y = 8 + i * RH;
      svg.append(s('text', { x: L - 6, y: y + 14, class: 'chart-label', 'text-anchor': 'end' }, r.label));
      svg.append(s('rect', { x: L, y: y + 3, width: x(100) - L, height: 14, rx: 3, class: 'chart-track' }));
      svg.append(s('rect', { x: L, y: y + 3, width: Math.max(1, x(r.value) - L), height: 14, rx: 3, class: 'chart-bar ' + (r.ok === false ? 'bad' : 'good') }));
      svg.append(s('text', { x: x(100) + 4, y: y + 14, class: 'chart-value' }, `${r.value.toFixed(1)}`));
    });
    if (typeof floor === 'number') {
      svg.append(s('line', { x1: x(floor), x2: x(floor), y1: 4, y2: H - 18, class: 'chart-floor' }));
      svg.append(s('text', { x: x(floor), y: H - 4, class: 'chart-label', 'text-anchor': 'middle' }, `floor ${floor} %`));
    }
    return svg;
  }

  function tile(label, value, sub, status) {
    return h('div', { class: 'tile ' + (status || '') }, h('span', { class: 'tile-label', text: label }),
      h('strong', { text: value }), sub ? h('span', { class: 'tile-sub', text: sub }) : null);
  }

  // -- Architecture ------------------------------------------------------------------------------------------
  function renderArchitecture(box, model, state) {
    const nodes = [];
    const node = (n, cls, text, sub) => {
      const b = h('button', { type: 'button', class: `node ${cls}${state.open === n.id ? ' open' : ''}`, 'data-node': n.id,
        'aria-pressed': String(state.open === n.id), 'aria-controls': 'arch-detail', onclick: () => { state.open = n.id; KB.views.render(); g.requestAnimationFrame(() => { const el = doc.querySelector(`[data-node="${n.id}"]`); if (el) el.focus(); }); } },
      h('strong', { text }), sub ? h('span', { text: sub }) : null);
      nodes.push(b);
      return b;
    };
    const ev = model.evidence;
    const partSub = (p) => (!p.used ? 'not built in this image' : ev && p.coverage ? `${p.tests} tests, branch ${pct(p.coverage.branch)}` : 'part of the image');
    const lanes = [
      ['Board and application', h('div', { class: 'lane-items' }, node({ id: 'board' }, 'tone-product', 'Board layer', 'your code: buses, handlers'))],
      ['Handlers and drivers', h('div', { class: 'lane-items' }, node(model.parts[0], 'tone-handler' + (model.parts[0].used ? '' : ' unused'), model.parts[0].label, partSub(model.parts[0])),
        ...model.drivers.map((d) => node(d, 'tone-driver', d.chip, `${d.version} on ${d.bus || 'no bus'}${d.model ? `, ${d.model}` : ''}`)))],
      ['Contracts and port', h('div', { class: 'lane-items' }, node(model.parts[1], 'tone-contract', model.parts[1].label, partSub(model.parts[1])),
        model.port ? node(model.port, 'tone-port', model.port.label, model.port.where) : null)],
      ['RTOS and DMA', h('div', { class: 'lane-items' }, node(model.parts[2], 'tone-rtos', model.parts[2].label, `${partSub(model.parts[2])}; backend ${model.rtos || '-'}`),
        node(model.parts[3], 'tone-contract' + (model.parts[3].used ? '' : ' unused'), model.parts[3].label, partSub(model.parts[3])))],
      ['Foundation', h('div', { class: 'lane-items' }, node(model.parts[4], 'tone-core', model.parts[4].label, partSub(model.parts[4])))],
    ];
    const map = h('div', { class: 'arch-map', role: 'group', 'aria-label': 'Layers of the image, top calls down' },
      lanes.map(([title, items]) => h('div', { class: 'lane' }, h('span', { class: 'lane-title', text: title }), items)));
    // Arrow keys move between the nodes in reading order (the buttons stay in the tab order too).
    map.addEventListener('keydown', (e) => {
      const i = nodes.indexOf(doc.activeElement);
      if (i < 0) return;
      const next = { ArrowRight: i + 1, ArrowDown: i + 1, ArrowLeft: i - 1, ArrowUp: i - 1 }[e.key];
      if (next === undefined) return;
      e.preventDefault();
      nodes[(next + nodes.length) % nodes.length].focus();
    });
    const detail = h('div', { id: 'arch-detail', class: 'arch-detail', role: 'region', 'aria-live': 'polite', 'aria-label': 'Details' });
    const p = model.parts.find((x) => x.id === state.open);
    const d = model.drivers.find((x) => x.id === state.open);
    if (p) {
      detail.append(h('h4', { text: p.label }), h('p', { text: p.holds }),
        h('dl', { class: 'facts' },
          h('dt', { text: 'Needs' }), h('dd', { text: p.needs.length ? p.needs.map((x) => `kb::${x}`).join(', ') : 'nothing (the foundation)' }),
          h('dt', { text: 'Needed by' }), h('dd', { text: p.neededBy.length ? p.neededBy.map((x) => `kb::${x}`).join(', ') : 'the board layer' }),
          h('dt', { text: 'In this image' }), h('dd', { text: p.used ? 'built' : 'not built (no driver needs it)' }),
          ...(ev && p.coverage ? [
            h('dt', { text: 'Unit tests' }), h('dd', { text: String(p.tests) }),
            h('dt', { text: 'Coverage' }), h('dd', { text: `line ${pct(p.coverage.line)}, branch ${pct(p.coverage.branch)}, MC/DC ${pct(p.coverage.condition)}` }),
            h('dt', { text: 'Mutants' }), h('dd', { text: p.mutation ? `${p.mutation.killed} killed, ${p.mutation.survived} survived (${pct(p.mutation.score)})` : '-' }),
            h('dt', { text: 'MISRA C++:2023' }), h('dd', { text: `in the scan of ${ev.release.version} (level ${ev.misra.level}, ${ev.misra.passed ? 'passed' : 'failed'})` }),
          ] : [h('dt', { text: 'Evidence' }), h('dd', { text: `none recorded for kb-core ${state.version || '-'}` })])),
        h('p', {}, link(DOCS + p.docs, `${p.label} in the docs`), ' · ', link(`${DOCS}api-reference/`, 'API reference')));
    } else if (d) {
      detail.append(h('h4', { text: `${d.chip} (driver-${d.name.replace(/_/g, '-')})` }),
        h('dl', { class: 'facts' }, h('dt', { text: 'Version' }), h('dd', { text: d.version }), h('dt', { text: 'Bus' }),
          h('dd', { text: `${d.bus || '-'}${d.model ? `, ${d.model}` : ''}` }), h('dt', { text: 'C++ floor' }), h('dd', { text: `C++${d.cxx}` }),
          h('dt', { text: 'Bound by' }), h('dd', { text: `kb/handlers/bindings/${d.name}.hpp` })),
        d.repo ? h('p', {}, link(`https://github.com/${d.repo}`, 'Repository and its own CI evidence')) : null);
    } else if (state.open === 'port' && model.port) {
      detail.append(h('h4', { text: model.port.label }), h('p', { text: `The MCU port: kb::interfaces contracts over the vendor SDK. ${model.port.where}.` }));
    } else if (state.open === 'board') {
      detail.append(h('h4', { text: 'Board layer' }), h('p', { text: 'Your code: it declares each bus with its thread model and binds a handler to each driver. The starter project carries it (board.cpp) with every selected binding included.' }));
    } else {
      detail.append(h('p', { class: 'hint', text: 'Pick a part, the port or a driver to see what it holds, what it needs and how it was tested.' }));
    }
    const twin = h('details', { class: 'twin' }, h('summary', { text: 'As a table' }),
      table('Parts of the image', ['Part', 'Needs', 'Built', 'Tests', 'Line', 'Branch', 'MC/DC', 'Mutation'],
        model.parts.map((x) => [x.label, x.needs.join(', ') || '-', x.used ? 'yes' : 'no', x.tests ?? '-',
          pct(x.coverage && x.coverage.line), pct(x.coverage && x.coverage.branch), pct(x.coverage && x.coverage.condition),
          pct(x.mutation && x.mutation.score)])));
    box.replaceChildren(h('div', { class: 'arch' }, map, detail), twin);
  }

  // -- Evidence -----------------------------------------------------------------------------------------------
  function renderEvidence(box, version) {
    const ev = evidenceFor(version);
    if (!ev) {
      box.replaceChildren(h('p', { class: 'hint', text: `No evidence is recorded for kb-core ${version || '(none selected)'}: only released versions have it (a development revision is tested by every pull request, but not recorded here).` }));
      return;
    }
    const c = ev.coverage; const m = ev.mutation; const mi = ev.misra;
    const st = (ok) => (ok ? 'ok' : 'bad');
    const tiles = h('div', { class: 'tiles' },
      tile('Unit tests', String(ev.tests.total), `${ev.tests.failures} failed`, st(ev.tests.failures === 0)),
      tile('Line coverage', pct(c.line.percent), `floor ${c.floors.line} %`, st(c.line.percent >= c.floors.line)),
      tile('Branch coverage', pct(c.branch.percent), `floor ${c.floors.branch} %`, st(c.branch.percent >= c.floors.branch)),
      tile('MC/DC', pct(c.condition.percent), 'GCC condition coverage', 'ok'),
      tile('Mutation score', pct(m.score), `${m.killed} killed, ${m.survived} survived`, st(m.threshold === null || m.score >= m.threshold)),
      tile('MISRA C++:2023', mi.passed ? 'Passed' : 'Failed', `${mi.findings.violations} advisory, ${mi.findings.deviated} deviated`, st(mi.passed)),
      tile('Fuzzing', `${ev.fuzz.planned_seconds} s`, `${ev.fuzz.targets.length} targets, ${ev.fuzz.result}`, st(ev.fuzz.result === 'pass')),
      tile('Sanitizers', ev.sanitizers.result === 'pass' ? 'Clean' : ev.sanitizers.result, ev.sanitizers.kinds.join(', '), st(ev.sanitizers.result === 'pass')));
    const covRows = c.parts.map((p) => ({ label: p.part, value: p.branch, ok: p.branch >= c.floors.branch }));
    const mutRows = (m.parts || []).map((p) => ({ label: p.part, value: p.score, ok: m.threshold === null || p.score >= m.threshold }));
    box.replaceChildren(
      h('p', { class: 'lead' }, `kb-core ${ev.release.version} (tag ${ev.release.tag}, ${(ev.release.commit || '').slice(0, 12)}): `,
        h('strong', { text: ev.passed ? 'every check passed.' : 'not every check passed.' }), ' From the CI run ',
        link(ev.source.run_url, String(ev.source.run_id)), ' of the release tag and its release assets; ',
        link(`${DOCS}assurance/evidence/`, 'the same on the docs site'), '.'),
      tiles,
      h('h4', { text: 'Test protocol and results' }),
      table('What CI checks for every release, its gate and this release\'s result', ['Check', 'Scope', 'Tool', 'Gate', 'Result', 'Status'],
        ev.protocol.map((p) => [p.check, p.scope, p.tool, p.gate, p.result, h('span', { class: 'badge ' + (p.status === 'pass' ? 'ok' : 'err'), text: p.status })])),
      h('div', { class: 'charts' },
        h('figure', {}, bars('Branch coverage by part, percent', covRows, c.floors.branch),
          h('figcaption', { text: 'Branch coverage by part (the table below has line, branch and MC/DC).' })),
        h('figure', {}, bars('Mutation score by part, percent', mutRows, m.threshold),
          h('figcaption', { text: 'Mutants killed by the tests, by part.' }))),
      table('Coverage, tests and mutants by part', ['Part', 'Tests', 'Line', 'Branch', 'MC/DC', 'Killed', 'Survived', 'Score'],
        c.parts.map((p) => {
          const mm = (m.parts || []).find((x) => x.part === p.part) || {};
          const tt = ev.tests.parts.find((x) => x.part === p.part) || {};
          return [p.part, tt.tests ?? '-', pct(p.line), pct(p.branch), pct(p.condition), mm.killed ?? '-', mm.survived ?? '-', pct(mm.score)];
        })),
      h('h4', { text: 'MISRA C++:2023' }),
      h('p', { text: `${mi.guidelines.checked} guidelines checked in ${mi.configurations.map((x) => `${x.name} (${x.translation_units} units)`).join(' and ')}: ${mi.guidelines.clean} clean, ${mi.guidelines.with_findings} with advisory findings (${mi.advisory_with_findings.join(', ') || 'none'}), ${mi.guidelines.deviated} under a deviation (${mi.findings.deviated} findings). Deviation records: ${mi.deviation_records.join(', ')}.` }),
      h('h4', { text: 'Documentation site' }),
      table('Lighthouse, mobile, median of three runs', ['Page', 'Performance', 'Accessibility', 'Best practices', 'SEO'],
        ev.lighthouse.pages.map((p) => [p.page, p.performance.toFixed(2), p.accessibility.toFixed(2), p.best_practices.toFixed(2), p.seo.toFixed(2)])),
      h('p', { class: 'note', text: `Links: ${ev.links.tool}, ${ev.links.result}. Fuzz targets: ${ev.fuzz.targets.join(', ')}. These are results of checks, not a safety claim.` }));
  }

  // -- Versions -----------------------------------------------------------------------------------------------
  function renderVersions(box, catalog, current) {
    const rows = versions(catalog);
    const fmt = (e, k, unit) => (e && typeof e[k] === 'number' ? `${e[k]}${unit || ''}` : '-');
    const dl = (row, k) => (row.delta && row.delta[k] ? ` (${row.delta[k] > 0 ? '+' : ''}${row.delta[k]})` : '');
    box.replaceChildren(
      h('p', { class: 'lead', text: rows.length > 1 ? 'Every kb-core version in the catalogue, newest first; a change from the version before is in brackets.' : 'The catalogue has one kb-core version so far: nothing to compare yet. The next release adds a row and the change from this one.' }),
      table('kb-core versions with their headline evidence', ['Version', 'Kind', 'Date', 'C++', 'Bindings', 'Tests', 'Branch', 'MC/DC', 'Mutation', 'All checks'],
        rows.map((r) => [h('span', {}, r.version, r.version === current ? h('span', { class: 'badge muted', text: 'selected' }) : null), r.kind,
          (r.date || '').slice(0, 10) || '-', `C++${r.cxx}`, String(r.bindings),
          fmt(r.evidence, 'tests') + dl(r, 'tests'), fmt(r.evidence, 'branch', ' %') + dl(r, 'branch'),
          fmt(r.evidence, 'mcdc', ' %') + dl(r, 'mcdc'), fmt(r.evidence, 'mutation', ' %') + dl(r, 'mutation'),
          r.evidence ? (r.evidence.passed ? 'passed' : 'failed') : 'not recorded'])));
  }

  // -- the step ----------------------------------------------------------------------------------------------
  const state = { tab: 'architecture', open: null, version: null };
  let last = null;
  function render(catalog, image, lock) {
    if (catalog) last = { catalog, image, lock };
    if (!last || !doc) return;
    const root = doc.getElementById('explore');
    if (!root) return;
    state.version = last.image.kb_core;
    doc.getElementById('explore-version').textContent = last.image.kb_core ? `kb-core ${last.image.kb_core}` : 'no kb-core version';
    const tabs = [['architecture', 'Architecture'], ['evidence', 'Evidence'], ['versions', 'Versions']];
    const list = doc.getElementById('explore-tabs');
    list.replaceChildren(...tabs.map(([id, label]) => h('button', { type: 'button', role: 'tab', id: 'xtab-' + id, 'aria-controls': 'explore-panel',
      'aria-selected': String(state.tab === id), tabindex: state.tab === id ? '0' : '-1', text: label,
      onclick: () => { state.tab = id; render(); doc.getElementById('xtab-' + id).focus(); },
      onkeydown: (e) => {
        const i = tabs.findIndex((t) => t[0] === state.tab);
        const j = e.key === 'ArrowRight' ? i + 1 : e.key === 'ArrowLeft' ? i - 1 : null;
        if (j === null) return;
        state.tab = tabs[(j + tabs.length) % tabs.length][0]; render(); doc.getElementById('xtab-' + state.tab).focus();
      } })));
    const panel = doc.getElementById('explore-panel');
    panel.setAttribute('aria-labelledby', 'xtab-' + state.tab);
    if (state.tab === 'architecture') renderArchitecture(panel, architecture(last.catalog, last.image, last.lock), state);
    else if (state.tab === 'evidence') renderEvidence(panel, last.image.kb_core);
    else renderVersions(panel, last.catalog, last.image.kb_core);
  }
  KB.views = { render, model: { PARTS, architecture, versions, evidenceFor } };
})(typeof globalThis !== 'undefined' ? globalThis : this);
