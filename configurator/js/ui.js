// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The page: reads the catalogue (data/catalog.js), keeps the selection in the URL hash, runs the resolver on every
// change and shows the findings and the files; opens an existing project's kb-image.yml and lock (read locally)
// and shows its upgrades with the shared planner. No network access, no cookies; the only stored value is the
// colour theme, in localStorage, when the browser allows it. Opened files stay in memory, never in the URL.
(function (g) {
  'use strict';
  const KB = g.KB;
  const doc = g.document;
  const catalog = g.KB_CATALOG;
  const { latestVersion } = KB.resolver;
  const { generate, zipProject } = KB.generate;
  const { encode, decode, flags } = KB.state;

  const $ = (id) => doc.getElementById(id);

  function h(tag, attrs, ...children) {
    const el = doc.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'text') el.textContent = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (v === true) el.setAttribute(k, '');
      else el.setAttribute(k, String(v));
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      el.append(c.nodeType ? c : doc.createTextNode(String(c)));
    }
    return el;
  }

  function announce(text) {
    const live = $('announce');
    live.textContent = '';
    g.setTimeout(() => { live.textContent = text; }, 30);
  }

  // Keep keyboard focus on the same control when a section is redrawn.
  function keepFocus(fn) {
    const id = doc.activeElement && doc.activeElement.id;
    fn();
    if (id && doc.activeElement && doc.activeElement.id !== id) {
      const el = $(id);
      if (el) el.focus({ preventScroll: true });
    }
  }

  const families = new Map(catalog.families.map((f) => [f.id, f]));
  const drivers = new Map(catalog.drivers.map((d) => [d.name, d]));
  const rtosById = new Map(catalog.rtos.map((r) => [r.id, r]));
  const kinds = catalog.bus_kinds;

  // -- release candidates: hidden unless the toggle is on (owner decision); the toggle state is in the hash ------
  let showRc = false;
  const isRc = (v) => v && v.kind === 'rc';
  function visibleVersions(versions, keep) {
    return (versions || []).filter((v) => showRc || !isRc(v) || v.version === keep);
  }
  function hasRc(sel) {
    const rcOf = (list, ver) => (list || []).some((v) => v.version === ver && isRc(v));
    const fam = families.get(sel.mcu.family);
    return rcOf(catalog.kb_core.releases, sel.kb_core) || (fam && fam.port && rcOf(fam.port.releases, sel.port))
      || sel.drivers.some((d) => drivers.has(d.name) && rcOf(drivers.get(d.name).versions, d.version));
  }
  // The version a driver gets when added: kb-core's pin or the newest release, or with the toggle on also an rc.
  // With a declared C++ standard, only versions that build with it are offered first (docs/language-floor.md).
  function pickVersion(d) {
    const fits = (v) => !image || !image.cxx || cxxOf(v) <= image.cxx;
    if (showRc) {
      const best = latestVersion(d);
      if (!best || fits(best)) return best;
      return d.versions.filter(fits).sort((a, b) => KB.core.compareVersions(b.version, a.version))[0] || best;
    }
    const rel = d.versions.filter((v) => !isRc(v) && fits(v));
    if (d.pinned) { const pinned = rel.find((v) => v.version === d.pinned); if (pinned) return pinned; }
    return rel.slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))[0] || null;
  }

  // -- selection -------------------------------------------------------------------------------------------------
  function newestVersion(all) {
    const releases = visibleVersions(all);
    if (!releases || !releases.length) return null;
    const finals = releases.filter((r) => r.kind === 'release');
    const pool = finals.length ? finals : releases;
    return pool.slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))[0].version;
  }

  function defaults() {
    const fam = families.get('esp32') || catalog.families[0];
    const board = fam.boards.find((b) => b.usable_gpio !== null) || fam.boards[0];
    return {
      image: 'my-image',
      kb_core: newestVersion(catalog.kb_core.releases),
      port: fam.port ? newestVersion(fam.port.releases) : null,
      mcu: { family: fam.id, target: board.target, board: board.id },
      rtos: fam.default_rtos,
      cxx: null,
      drivers: [],
      buses: {},
    };
  }

  function busCount(kind) {
    const fam = families.get(image.mcu.family);
    const target = fam && fam.targets.find((t) => t.id === image.mcu.target);
    const n = target ? target.buses[kind] : null;
    return n === null || n === undefined ? 8 : n;
  }

  function defaultVia(version) {
    return version.bus.find((b) => kinds[b] && busCount(kinds[b].instance) > 0) || version.bus[0];
  }

  function defaultBus(via, exclude) {
    const kind = kinds[via];
    if (!kind) return '';
    const inst = kind.instance;
    if (inst === 'adc' || inst === 'gpio') return inst + '0';
    const count = busCount(inst);
    const used = new Set(image.drivers.filter((d) => d !== exclude).map((d) => d.bus));
    if (kind.shared) {
      // Reuse a bus that already runs the same protocol, else the first free one.
      const same = image.drivers.find((d) => d !== exclude && d.via === via);
      if (same) return same.bus;
      for (let i = 0; i < Math.max(count, 1); i++) if (!used.has(inst + i)) return inst + i;
      return inst + '0';
    }
    const start = inst === 'uart' && count > 1 ? 1 : 0; // uart0 is usually the console
    for (let i = start; i < Math.max(count, 1); i++) if (!used.has(inst + i)) return inst + i;
    return inst + count;
  }

  function defaultModel(busId) {
    const onBus = image.drivers.filter((d) => d.bus === busId);
    if (onBus.some((d) => drivers.get(d.name) && drivers.get(d.name).requires_owner)) return 'owner';
    const via = onBus[0] && onBus[0].via;
    return (kinds[via] && kinds[via].default_model) || 'owner';
  }

  // Fill what is missing after a change; never overrides a choice the user made (the resolver reports those).
  function normalize() {
    let fam = families.get(image.mcu.family);
    if (!fam) { fam = catalog.families[0]; image.mcu.family = fam.id; }
    if (!fam.targets.some((t) => t.id === image.mcu.target)) image.mcu.target = fam.targets[0].id;
    const board = fam.boards.find((b) => b.id === image.mcu.board);
    if (!board || board.target !== image.mcu.target) {
      const pick = fam.boards.filter((b) => b.target === image.mcu.target);
      image.mcu.board = (pick.find((b) => b.usable_gpio !== null) || pick[0] || { id: null }).id;
    }
    if (!fam.rtos.includes(image.rtos)) image.rtos = fam.default_rtos;
    if (!fam.port || !fam.port.releases.length) image.port = null;
    else if (image.port && !fam.port.releases.some((r) => r.version === image.port)) image.port = newestVersion(fam.port.releases);
    image.image = KB.core.safeName(image.image);
    const used = new Set();
    for (const d of image.drivers) used.add(d.bus);
    for (const id of used) if (!image.buses[id]) image.buses[id] = { model: defaultModel(id) };
    for (const id of Object.keys(image.buses)) if (!used.has(id)) delete image.buses[id];
  }

  let image = decode(g.location.hash) || defaults();
  showRc = flags(g.location.hash).rc || hasRc(image);
  let result = null;
  let output = null;
  let activeTab = 'image';

  // -- step 1 and 2 --------------------------------------------------------------------------------------------
  function renderFamilies() {
    const box = $('families');
    box.replaceChildren(box.querySelector('legend') || h('legend', { text: 'MCU family' }));
    for (const fam of catalog.families) {
      const id = 'fam-' + fam.id;
      box.append(h('div', { class: 'choice' },
        h('input', { type: 'radio', name: 'family', id, value: fam.id, checked: image.mcu.family === fam.id,
          'aria-describedby': id + '-d', onchange: () => { image.mcu.family = fam.id; image.mcu.target = ''; image.port = fam.port ? newestVersion(fam.port.releases) : null; update(true); } }),
        h('label', { class: 'face', for: id },
          h('strong', {}, fam.label, ' ', fam.status === 'planned' ? h('span', { class: 'badge planned', text: 'planned' }) : null),
          h('span', { id: id + '-d', text: fam.id === 'host' ? 'Built into kb-core: test doubles with fault injection'
            : fam.port ? `Port ${fam.port.name}, ${fam.targets.length} targets` : `No port yet${fam.port_planned ? ` (${fam.port_planned})` : ''}` }))));
    }
  }

  function renderTargets() {
    const fam = families.get(image.mcu.family);
    const sel = $('target');
    sel.replaceChildren(...fam.targets.map((t) => h('option', { value: t.id, selected: t.id === image.mcu.target, text: t.label })));
    const boards = $('board');
    boards.replaceChildren(...fam.boards.filter((b) => b.target === image.mcu.target)
      .map((b) => h('option', { value: b.id, selected: b.id === image.mcu.board, text: b.label })));
    const board = fam.boards.find((b) => b.id === image.mcu.board);
    const target = fam.targets.find((t) => t.id === image.mcu.target);
    const parts = [];
    if (target) {
      const b = target.buses;
      parts.push(['i2c', 'spi', 'uart', 'can', 'pio'].filter((k) => b[k]).map((k) => `${b[k]} ${k === 'pio' ? 'RMT' : k.toUpperCase()}`).join(', '));
    }
    parts.push(board && board.usable_gpio !== null ? `about ${board.usable_gpio} free GPIO` : 'free pins not known for this board');
    $('board-hint').textContent = parts.join('; ') + '.';
  }

  function renderRtos() {
    const fam = families.get(image.mcu.family);
    const box = $('rtos');
    box.replaceChildren(box.querySelector('legend') || h('legend', { text: 'RTOS backend' }));
    for (const r of catalog.rtos) {
      const id = 'rtos-' + r.id;
      const allowed = fam.rtos.includes(r.id);
      const why = !allowed ? `Not possible on ${fam.label}` : r.status === 'planned' ? 'Planned backend, not released' : `KB_RTOS=${r.kb_rtos}`;
      box.append(h('div', { class: 'choice' },
        h('input', { type: 'radio', name: 'rtos', id, value: r.id, checked: image.rtos === r.id, disabled: !allowed,
          'aria-describedby': id + '-d', onchange: () => { image.rtos = r.id; update(); } }),
        h('label', { class: 'face', for: id },
          h('strong', {}, r.label, ' ', r.status === 'planned' ? h('span', { class: 'badge planned', text: 'planned' }) : null),
          h('span', { id: id + '-d', text: why }))));
    }
    $('rtos-note').textContent = fam.rtos_note || 'The RTOS backend of kb::rtos.';
  }

  // The C++ standard a version needs (its catalogue language block, else the registry default C++20).
  function cxxOf(v) {
    return v && v.language && typeof v.language.standard === 'number' ? v.language.standard : 20;
  }
  function verText(v) {
    return `${v.version}${v.kind === 'rc' ? ' (rc)' : ''}, C++${cxxOf(v)}`;
  }

  function versionOptions(releases, current, none) {
    const opts = visibleVersions(releases, current).slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))
      .map((r) => h('option', { value: r.version, selected: r.version === current,
        text: `${r.version}${r.kind === 'rc' ? ' (release candidate)' : r.kind === 'dev' ? ` (development revision ${r.commit.slice(0, 7)}, no release yet)` : ''}` }));
    if (!opts.length) opts.push(h('option', { value: '', selected: true, text: none }));
    return opts;
  }

  function renderVersions() {
    $('kb-version').replaceChildren(...versionOptions(catalog.kb_core.releases, image.kb_core, 'None in the catalogue'));
    const fam = families.get(image.mcu.family);
    const port = $('port-version');
    if (!fam.port || !fam.port.releases.length) {
      port.replaceChildren(h('option', { value: '', text: fam.port ? `Inside kb-core (${fam.port.path})` : 'No port yet' }));
      port.disabled = true;
    } else {
      port.disabled = false;
      port.replaceChildren(...versionOptions(fam.port.releases, image.port, `${fam.port.repo.split('/')[1]}: no release yet`));
    }
    const name = $('image-name');
    if (doc.activeElement !== name) name.value = image.image;
    $('cxx').replaceChildren(
      h('option', { value: '', selected: !image.cxx, text: 'Not declared (no check)' }),
      ...[20, 23].map((n) => h('option', { value: String(n), selected: image.cxx === n, text: `C++${n}` })));
  }

  // -- step 3: the catalogue ---------------------------------------------------------------------------------
  const filter = { text: '', buses: new Set() };
  const cards = new Map();

  function haystack(d) {
    return [d.name, d.chip, d.package, d.summary, ...(d.versions[0] ? d.versions[0].bus : []),
      ...d.capabilities.map((c) => c.label + ' ' + c.text)].join(' ').toLowerCase();
  }

  function openFigures(d, trigger) {
    const dlg = $('figure-dialog');
    $('figure-title').textContent = `${d.chip}: figures`;
    $('figure-body').replaceChildren(...d.figures.map((f) => h('figure', {},
      h('img', { class: 'fig-light', src: f.light, alt: f.alt, loading: 'lazy', decoding: 'async' }),
      h('img', { class: 'fig-dark', src: f.dark, alt: f.alt, loading: 'lazy', decoding: 'async' }),
      h('figcaption', { text: `${f.kind[0].toUpperCase()}${f.kind.slice(1)}. From the docs of ${d.repo.split('/')[1]}.` }))));
    dlg.returnFocus = trigger;
    dlg.showModal();
  }

  function selected(name) {
    return image.drivers.find((d) => d.name === name) || null;
  }

  function buildCatalogue() {
    const list = $('catalogue');
    const busValues = [...new Set(catalog.drivers.flatMap((d) => (d.versions[0] ? d.versions[0].bus : [])))].sort();
    $('bus-filters').replaceChildren(...busValues.map((b) => h('button', {
      type: 'button', 'aria-pressed': 'false', id: 'filter-' + b, text: (kinds[b] && kinds[b].label.split(' ')[0]) || b,
      onclick: (e) => {
        if (filter.buses.has(b)) filter.buses.delete(b); else filter.buses.add(b);
        e.currentTarget.setAttribute('aria-pressed', String(filter.buses.has(b)));
        applyFilter();
      },
    })));
    for (const d of catalog.drivers) {
      const titleId = 'dn-' + d.name;
      const fig = d.figures[0];
      const thumb = fig
        ? h('button', { type: 'button', class: 'thumb', 'aria-label': `Show the figures of ${d.chip}`, onclick: (e) => openFigures(d, e.currentTarget) },
          h('img', { class: 'fig-light', src: fig.light, alt: '', loading: 'lazy', decoding: 'async', width: 240, height: 134 }),
          h('img', { class: 'fig-dark', src: fig.dark, alt: '', loading: 'lazy', decoding: 'async', width: 240, height: 134 }))
        : h('div', { class: 'thumb empty', text: 'No figure published' });
      const latest = pickVersion(d);
      const versionSel = h('select', { id: 'ver-' + d.name, disabled: !d.versions.length, 'aria-label': `Version of ${d.name}`,
        onchange: (e) => {
          const s = selected(d.name);
          if (s) {
            s.version = e.target.value;
            const v = d.versions.find((x) => x.version === s.version);
            if (!v.bus.includes(s.via)) { s.via = defaultVia(v); s.bus = defaultBus(s.via, s); }
            update();
          }
        } },
      d.versions.length ? d.versions.map((v) => h('option', { value: v.version, selected: latest && v.version === latest.version, text: verText(v) }))
        : h('option', { text: 'none published' }));
      const add = h('button', { type: 'button', class: 'add', id: 'add-' + d.name, 'aria-pressed': 'false', disabled: !d.versions.length,
        text: 'Add',
        onclick: () => toggleDriver(d, versionSel.value) });
      const rcNote = h('p', { class: 'sub rc-note', hidden: true, text: 'Release candidates only: turn on "Show release candidates" to add it.' });
      const v0 = d.versions[0];
      const status = d.status === 'unpublished' ? h('span', { class: 'badge unpublished', text: 'not published' })
        : d.status === 'rc' ? h('span', { class: 'badge rc', text: 'rc' }) : h('span', { class: 'badge release', text: 'release' });
      const card = h('li', { class: 'card', id: 'card-' + d.name, 'aria-labelledby': titleId },
        thumb,
        h('div', { class: 'card-body' },
          h('h3', { id: titleId }, d.chip, status),
          h('p', { class: 'sub', text: `${d.name}${d.package ? ' · ' + d.package : ''}` }),
          h('p', { class: 'summary', text: d.summary || 'This driver has no public release yet, so it cannot be pinned.' }),
          h('ul', { class: 'tags', 'aria-label': 'Buses' }, (v0 ? v0.bus : []).map((b) => h('li', { class: 'badge bus', text: (kinds[b] && kinds[b].label) || b }))),
          d.capabilities.length ? h('details', {}, h('summary', { text: 'Capabilities' }),
            h('dl', {}, d.capabilities.map((c) => [h('dt', { text: c.label }), h('dd', { text: c.text })]))) : null,
          h('div', { class: 'meta' },
            h('span', { text: d.licence.spdx }),
            v0 && !v0.mcus.includes('any') ? h('span', { class: 'badge warn', text: `${v0.mcus.join(', ')} only` }) : null,
            d.requires_owner ? h('span', { class: 'badge muted', text: 'needs owner task' }) : null),
          h('div', { class: 'actions' }, h('div', { class: 'field' }, versionSel), add), rcNote));
      cards.set(d.name, { card, add, versionSel, rcNote, text: haystack(d), d });
      list.append(card);
    }
    $('search').addEventListener('input', (e) => { filter.text = e.target.value; applyFilter(); });
    applyFilter();
  }

  function applyFilter() {
    const tokens = filter.text.toLowerCase().split(/\s+/).filter(Boolean);
    let shown = 0;
    for (const { card, text, d } of cards.values()) {
      const buses = d.versions[0] ? d.versions[0].bus : [];
      const ok = tokens.every((t) => text.includes(t)) && (!filter.buses.size || buses.some((b) => filter.buses.has(b)));
      card.hidden = !ok;
      if (ok) shown++;
    }
    $('count').textContent = `${shown} of ${cards.size} drivers shown, ${image.drivers.length} selected.`;
    // No match for a search: offer the intake form, prefilled with the search text reduced to a part-number alphabet.
    const ask = $('request-chip');
    const part = filter.text.replace(/[^A-Za-z0-9._-]/g, '').slice(0, 40);
    ask.hidden = shown > 0 || !part;
    if (!ask.hidden) {
      ask.replaceChildren(`No driver for ${part}: `, h('a', {
        href: `${REQUESTS}?template=request-driver.yml&part=${encodeURIComponent(part)}`, target: '_blank', rel: 'noopener noreferrer',
        text: 'request it' }), ' (opens the request form on GitHub).');
    }
  }

  const REQUESTS = 'https://github.com/kokeb-labs-drivers/driver-requests/issues/new';

  function toggleDriver(d, version) {
    const s = selected(d.name);
    if (s) {
      image.drivers = image.drivers.filter((x) => x !== s);
      announce(`${d.chip} removed.`);
    } else {
      const v = d.versions.find((x) => x.version === version) || pickVersion(d);
      if (!v) return;
      const entry = { name: d.name, version: v.version, via: defaultVia(v), bus: '' };
      entry.bus = defaultBus(entry.via, entry);
      image.drivers.push(entry);
      image.drivers.sort((a, b) => (a.name < b.name ? -1 : 1));
      announce(`${d.chip} added on ${entry.bus}.`);
    }
    update();
  }

  function updateCards() {
    for (const { card, add, versionSel, rcNote, d } of cards.values()) {
      const s = selected(d.name);
      const shownVersions = visibleVersions(d.versions, s ? s.version : null);
      const want = s ? s.version : (pickVersion(d) || {}).version;
      versionSel.replaceChildren(...(shownVersions.length
        ? shownVersions.map((v) => h('option', { value: v.version, selected: v.version === want, text: verText(v) }))
        : [h('option', { value: '', text: d.versions.length ? 'release candidates only' : 'none published' })]));
      versionSel.disabled = !shownVersions.length;
      add.disabled = !shownVersions.length && !s;
      rcNote.hidden = shownVersions.length > 0 || !d.versions.length;
      card.dataset.selected = String(!!s);
      add.setAttribute('aria-pressed', String(!!s));
      add.replaceChildren(s ? 'Added' : 'Add', h('span', { class: 'visually-hidden', text: ` ${d.chip}` }));
      if (s && versionSel.value !== s.version) versionSel.value = s.version;
    }
    applyFilter();
  }

  // -- step 4: buses, handlers, thread models --------------------------------------------------------------
  function busOptions(via, current) {
    const kind = kinds[via];
    if (!kind) return [h('option', { value: current, text: current })];
    const inst = kind.instance;
    const n = inst === 'adc' || inst === 'gpio' ? 1 : busCount(inst);
    const ids = [];
    for (let i = 0; i < n; i++) ids.push(inst + i);
    if (current && !ids.includes(current)) ids.push(current);
    return ids.map((id) => h('option', { value: id, selected: id === current, text: id + (ids.indexOf(id) >= n ? ' (not on this target)' : '') }));
  }

  function renderAssign() {
    const box = $('assign');
    if (!image.drivers.length) {
      box.replaceChildren(h('p', { class: 'empty-note', text: 'Add drivers in step 3; each one appears here with its bus.' }));
      return;
    }
    const rows = image.drivers.map((s) => {
      const d = drivers.get(s.name);
      const v = d && d.versions.find((x) => x.version === s.version);
      const vias = v ? v.bus : [s.via];
      return h('tr', {},
        h('td', {}, h('strong', { text: d ? d.chip : s.name }), ' ', h('span', { class: 'sub', text: s.name })),
        h('td', {}, h('label', { class: 'visually-hidden', for: 'via-' + s.name, text: `Bus kind of ${s.name}` }),
          h('select', { id: 'via-' + s.name, disabled: vias.length < 2, onchange: (e) => { s.via = e.target.value; s.bus = defaultBus(s.via, s); update(); } },
            vias.map((b) => h('option', { value: b, selected: b === s.via, text: (kinds[b] && kinds[b].label) || b })))),
        h('td', {}, h('label', { class: 'visually-hidden', for: 'bus-' + s.name, text: `Bus of ${s.name}` }),
          h('select', { id: 'bus-' + s.name, onchange: (e) => { s.bus = e.target.value; update(); } }, busOptions(s.via, s.bus))),
        h('td', {}, d && d.requires_owner ? h('span', { class: 'badge muted', text: 'owner task only' }) : h('span', { class: 'sub', text: 'either model' })),
        h('td', {}, h('button', { type: 'button', class: 'ghost', id: 'rm-' + s.name, 'aria-label': `Remove ${s.name}`, text: 'Remove',
          onclick: () => { image.drivers = image.drivers.filter((x) => x !== s); announce(`${s.name} removed.`); update(); } })));
    });
    box.replaceChildren(h('div', { class: 'table-wrap' }, h('table', { class: 'assign' },
      h('caption', { class: 'visually-hidden', text: 'Bus of each selected driver' }),
      h('thead', {}, h('tr', {}, ['Driver', 'Talks over', 'Bus', 'Handler'].map((t) => h('th', { scope: 'col', text: t })), h('th', { scope: 'col' }, h('span', { class: 'visually-hidden', text: 'Action' })))),
      h('tbody', {}, rows))));
  }

  function renderBuses() {
    const box = $('buses');
    const buses = result.resolved.buses;
    box.replaceChildren(...Object.keys(buses).map((id) => {
      const b = buses[id];
      const needsModel = true;
      const owners = b.devices.filter((n) => drivers.get(n) && drivers.get(n).requires_owner);
      const model = image.buses[id] ? image.buses[id].model : null;
      const radio = (value, title, text) => h('label', {},
        h('input', { type: 'radio', name: 'model-' + id, id: `model-${id}-${value}`, value, checked: model === value,
          onchange: () => { image.buses[id] = { model: value }; update(); } }),
        h('span', {}, h('strong', { text: title }), ' ', text));
      return h('section', { class: 'bus', 'aria-labelledby': 'bus-h-' + id },
        h('h3', { id: 'bus-h-' + id }, id, h('span', { class: 'badge bus', text: b.via.map((x) => ((kinds[x] && kinds[x].label) || x).split(' (')[0]).join(', ') })),
        h('p', { text: `${b.devices.length} driver${b.devices.length === 1 ? '' : 's'}: ${b.devices.join(', ')}` +
          (b.addresses ? `; addresses ${Object.keys(b.addresses).map((n) => `${n} 0x${b.addresses[n].toString(16).toUpperCase().padStart(2, '0')}`).join(', ')}` : '') }),
        needsModel
          ? h('fieldset', { class: 'models' }, h('legend', { text: 'Thread model' }),
            radio('owner', 'Owner task.', 'One task serves every device and publishes snapshots; no mutex.'),
            radio('guarded', 'Guarded facade.', 'Each call takes the handler and bus locks with a timeout.'),
            owners.length ? h('p', { text: `${owners.join(', ')} must be serviced continuously and needs an owner task.` }) : null)
          : null);
    }));
    const pins = result.resolved.pins;
    const box2 = $('pins');
    if (!image.drivers.length) { box2.replaceChildren(); return; }
    const budget = pins.budget;
    const label = budget !== null
      ? `Pins: ${pins.min} needed, ${pins.optional} optional control pins, about ${budget} free on this board.`
      : `Pins: ${pins.min} needed, ${pins.optional} optional control pins; this board's free pins are not known.`;
    const meter = h('div', { class: 'meter' + (budget !== null && pins.min > budget ? ' over' : ''), role: 'img', 'aria-label': label });
    if (budget) {
      const opt = h('span', { class: 'opt' });
      const min = h('span', {});
      opt.style.width = Math.min(100, ((pins.min + pins.optional) / budget) * 100) + '%';
      min.style.width = Math.min(100, (pins.min / budget) * 100) + '%';
      meter.append(opt, min);
    }
    box2.replaceChildren(h('span', { text: label }), budget ? meter : null);
  }

  // -- result: findings, evidence, files ----------------------------------------------------------------------
  function renderFindings() {
    const n = { error: 0, warning: 0, info: 0 };
    for (const f of result.findings) n[f.severity]++;
    $('summary').replaceChildren(
      n.error ? h('span', { class: 'badge err', text: `${n.error} error${n.error === 1 ? '' : 's'}` }) : h('span', { class: 'badge ok', text: 'No errors' }),
      h('span', { class: 'badge warn', text: `${n.warning} warning${n.warning === 1 ? '' : 's'}` }),
      h('span', { class: 'badge muted', text: `${n.info} note${n.info === 1 ? '' : 's'}` }));
    const icon = { error: '!', warning: '!', info: 'i' };
    $('findings').replaceChildren(...result.findings.map((f) => h('li', { class: 'finding ' + f.severity },
      h('span', { class: 'icon', 'aria-hidden': 'true', text: icon[f.severity] }),
      h('span', {}, h('span', { class: 'visually-hidden', text: f.severity + ': ' }), h('code', { text: f.code }),
        f.mirrors ? h('code', { text: f.mirrors, title: 'The shared kb rule this check mirrors' }) : null, f.message))));
    const ctx = result.context;
    const tested = result.resolved.tested_sets.length ? `Tested (${result.resolved.tested_sets.join(', ')})` : 'Untested combination';
    const comps = output.rows;
    const scanned = comps.filter((c) => c.osv);
    const findings = scanned.reduce((a, c) => a + c.osv.findings.length, 0);
    const cve = !scanned.length ? 'Not scanned' : scanned.length < comps.length
      ? `${findings} known, ${comps.length - scanned.length} not scanned` : findings ? `${findings} known` : 'None known';
    const licences = [...new Set(comps.map((c) => c.licence).concat(output.soup.map((s) => s.licence)))].sort().join(', ') || 'none';
    const count = (k) => comps.filter((c) => c.kind === k).length;
    const pins = [['release', count('release')], ['rc', count('rc')], ['dev', count('dev')]].filter(([, n]) => n).map(([k, n]) => `${n} ${k}`).join(', ') || 'none';
    $('evidence').replaceChildren(
      ...[['Tested set', tested], ['Known CVEs', cve], ['Licences', licences], ['Pins', pins]]
        .map(([k, v]) => h('div', {}, h('dt', { text: k }), h('dd', { text: v }))));
    void ctx;
  }

  function download(name, data, type) {
    const blob = new Blob([data], { type });
    const url = URL.createObjectURL(blob);
    const a = h('a', { href: url, download: name });
    doc.body.append(a);
    a.click();
    a.remove();
    g.setTimeout(() => URL.revokeObjectURL(url), 2000);
  }

  async function copy(text, what) {
    try {
      await g.navigator.clipboard.writeText(text);
    } catch (e) {
      const ta = h('textarea', { class: 'visually-hidden' });
      ta.value = text;
      doc.body.append(ta);
      ta.select();
      doc.execCommand('copy');
      ta.remove();
    }
    announce(`${what} copied.`);
  }

  function renderOutputs() {
    const tabs = $('tabs');
    const panels = $('tabpanels');
    if (!output.files.some((f) => f.id === activeTab)) activeTab = output.files[0].id;
    tabs.replaceChildren(...output.files.map((f) => h('button', {
      type: 'button', role: 'tab', id: 'tab-' + f.id, 'aria-controls': 'panel-' + f.id,
      'aria-selected': String(f.id === activeTab), tabindex: f.id === activeTab ? '0' : '-1', text: f.label,
      onclick: () => { activeTab = f.id; renderOutputs(); $('tab-' + f.id).focus(); },
    })));
    const f = output.files.find((x) => x.id === activeTab);
    panels.replaceChildren(h('div', { class: 'tabpanel', role: 'tabpanel', id: 'panel-' + f.id, 'aria-labelledby': 'tab-' + f.id },
      h('div', { class: 'file-actions' },
        h('span', { class: 'path', text: f.path }),
        h('span', {},
          h('button', { type: 'button', id: 'copy-file', text: 'Copy', onclick: () => copy(f.text, f.path) }), ' ',
          h('button', { type: 'button', id: 'save-file', text: 'Download', onclick: () => download(f.path.split('/').pop(), f.text, 'text/plain;charset=utf-8') }))),
      h('pre', { class: 'code', tabindex: '0', 'aria-label': `Contents of ${f.path}` }, h('code', { text: f.text }))));
    const zip = $('zip');
    zip.disabled = !output.ready;
    $('zip-note').textContent = output.ready
      ? `Holds every file above plus a ${image.mcu.family === 'esp32' ? 'ESP-IDF' : 'CMake'} project skeleton; built in your browser, repeatable byte for byte.`
      : 'Fix the errors first: the archive is only offered for a selection that resolves.';
  }

  function onTabKey(e) {
    const ids = output.files.map((f) => f.id);
    let i = ids.indexOf(activeTab);
    if (e.key === 'ArrowRight') i = (i + 1) % ids.length;
    else if (e.key === 'ArrowLeft') i = (i - 1 + ids.length) % ids.length;
    else if (e.key === 'Home') i = 0;
    else if (e.key === 'End') i = ids.length - 1;
    else return;
    e.preventDefault();
    activeTab = ids[i];
    renderOutputs();
    $('tab-' + activeTab).focus();
  }


  // -- open an existing project and its upgrades ---------------------------------------------------------------
  // The files stay in memory only; the plan is the shared planner's (resolver.mjs plan, the rules of kb upgrade).
  let opened = null;
  let targets = {};
  let upgradePlan = null;
  let lastGood = null; // the last plan that worked: its rows stay on screen while a chosen version is refused

  function allowRc() {
    return showRc;
  }

  function kbCat() {
    return KB.shared.kbCatalog(catalog);
  }

  function computePlan() {
    if (!opened || !KB.engine) return null;
    return KB.engine.plan(kbCat(), opened.image, { lock: opened.lock, to: targets, allowRc: allowRc() });
  }

  // Missing bus kinds and buses get the page's defaults, as when a driver is added by hand.
  function fillSelection(sel) {
    for (const d of sel.drivers) {
      const drv = drivers.get(d.name);
      if (!drv || !drv.versions.length) continue;
      let v = drv.versions.find((x) => x.version === d.version);
      if (!v) { v = pickVersion(drv) || latestVersion(drv); d.version = v.version; }
      if (!d.via || !v.bus.includes(d.via)) d.via = defaultVia(v);
      if (!d.bus) d.bus = defaultBus(d.via, d);
    }
    return sel;
  }

  function versionsOf(row) {
    const cat = kbCat();
    if (row.kind === 'kb-core') return cat.kb_core.versions;
    if (row.kind === 'port') {
      const fam = cat.mcus.find((m) => m.port && `port-${m.port.name}` === row.name);
      return fam ? fam.port.versions : [];
    }
    const d = cat.drivers.find((x) => x.name === row.name);
    return d ? d.versions : [];
  }

  function vulnText(ids) {
    return ids === null ? 'not scanned' : ids.length ? ids.join(', ') : 'none known';
  }

  function renderUpgrade() {
    const box = $('upgrade');
    if (!opened) { box.hidden = true; return; }
    box.hidden = false;
    upgradePlan = computePlan();
    const p = upgradePlan;
    const sum = $('upgrade-summary');
    const buttons = ['upgrade-apply', 'upgrade-report', 'upgrade-image', 'upgrade-lock'];
    if (!p) {
      sum.replaceChildren(h('span', { class: 'badge muted', text: 'The shared resolver is still loading' }));
      $('upgrade-table').replaceChildren();
      $('upgrade-findings').replaceChildren();
      for (const id of buttons) $(id).disabled = true;
      return;
    }
    for (const id of buttons) $(id).disabled = !p.ok;
    if (p.ok) lastGood = p;
    $('upgrade-clear').hidden = !Object.keys(targets).length;
    // A refused choice (a held-back or older version) keeps the last good rows, so the choice can be changed back.
    const shown = p.ok ? p : lastGood && Object.keys(targets).length ? lastGood : p;
    const changed = p.rows.filter((r) => r.change !== 'none');
    sum.replaceChildren(...[
      p.ok ? h('span', { class: changed.length ? 'badge warn' : 'badge ok', text: changed.length ? `${changed.length} of ${p.rows.length} components can move` : 'Up to date' })
        : h('span', { class: 'badge err', text: 'Cannot plan an upgrade' }),
      p.rows.some((r) => r.breaking) ? h('span', { class: 'badge err', text: 'breaking changes' }) : null,
      p.rows.some((r) => r.fixed.length) ? h('span', { class: 'badge ok', text: 'fixes known vulnerabilities' }) : null,
      h('span', { class: 'badge muted', text: `from ${opened.label}` })].filter(Boolean));
    const head = ['Component', 'Current', 'Upgrade to', 'Change', 'Newest release', 'Known vulnerabilities', 'Held back'];
    const rows = shown.rows.map((r) => {
      const heldWhy = new Map(r.held.map((x) => [x.version, KB.engine.HELD_TEXT[x.code] || x.code]));
      const opts = versionsOf(r).filter((v) => v.kind !== 'dev' || v.version === r.to)
        .filter((v) => showRc || v.kind !== 'rc' || v.version === r.from || v.version === targets[r.name])
        .filter((v) => KB.engine.compareVersions(v.version, r.from) >= 0)
        .sort((a, b) => KB.engine.compareVersions(b.version, a.version));
      const sel = h('select', { id: 'to-' + r.name, 'aria-label': `Upgrade ${r.name} to`,
        onchange: (e) => { if (e.target.value) targets[r.name] = e.target.value; else delete targets[r.name]; renderUpgrade(); announce(`${r.name}: ${e.target.value || 'newest selectable'}.`); } },
      h('option', { value: '', selected: !targets[r.name], text: 'newest selectable' }),
      opts.map((v) => h('option', { value: v.version, selected: targets[r.name] === v.version, disabled: heldWhy.has(v.version) && targets[r.name] !== v.version,
        text: `${v.version}${v.kind === 'rc' ? ' (rc)' : v.kind === 'dev' ? ' (dev)' : ''}${v.version === r.from ? ' (current)' : ''}${heldWhy.has(v.version) ? `: held back, ${heldWhy.get(v.version)}` : ''}` })));
      return h('tr', {},
        h('td', {}, h('strong', { text: r.name })),
        h('td', {}, r.from, r.from_kind === 'rc' ? h('span', { class: 'badge rc', text: 'rc' }) : null),
        h('td', {}, h('div', {}, h('strong', { text: r.to }), ' ', r.to_kind === 'rc' ? h('span', { class: 'badge rc', text: 'rc' }) : null), sel),
        h('td', {}, r.change === 'none' ? h('span', { class: 'sub', text: 'none' }) : h('span', { class: r.breaking ? 'badge err' : 'badge ok', text: r.breaking ? `${r.change}, breaking` : r.change }),
          r.notes ? h('div', {}, h('a', { href: r.notes, rel: 'noreferrer noopener', target: '_blank', text: 'release notes' })) : null),
        h('td', { text: `${r.newest_release || 'none'}${r.newest_rc ? `; rc ${r.newest_rc}` : ''}` }),
        h('td', { text: r.change === 'none' ? vulnText(r.osv_from) : `${vulnText(r.osv_from)} → ${vulnText(r.osv_to)}` }),
        h('td', { text: r.held.map((x) => `${x.version}: ${KB.engine.HELD_TEXT[x.code] || x.code}`).join('; ') || '-' }));
    });
    $('upgrade-table').replaceChildren(shown.rows.length ? h('div', { class: 'table-wrap' }, h('table', { class: 'assign upgrade' },
      h('caption', { class: 'visually-hidden', text: 'Available upgrades per component' }),
      h('thead', {}, h('tr', {}, head.map((t) => h('th', { scope: 'col', text: t })))),
      h('tbody', {}, rows))) : null);
    const icon = { error: '!', warning: '!' };
    $('upgrade-findings').replaceChildren(...[...p.errors.map((e) => ['error', e]), ...p.warnings.map((w) => ['warning', w])]
      .map(([sev, f]) => h('li', { class: 'finding ' + sev },
        h('span', { class: 'icon', 'aria-hidden': 'true', text: icon[sev] }),
        h('span', {}, h('span', { class: 'visually-hidden', text: sev + ': ' }), h('code', { text: f.code }), `${f.message} (${f.subject})`))));
  }

  function upgradeFile(name) {
    if (!upgradePlan || !upgradePlan.ok) return;
    const text = KB.engine.upgradeFiles(upgradePlan, allowRc())[name];
    download(name, text, 'text/plain;charset=utf-8');
    announce(`${name} downloaded.`);
  }

  function applyUpgrade() {
    if (!upgradePlan || !upgradePlan.ok) return;
    const p = upgradePlan;
    const versions = new Map(p.lock.drivers.map((d) => [d.name, d.version]));
    image.kb_core = p.lock.kb_core.version;
    const port = p.lock.ports[0];
    if (port && port.in_tree === false) image.port = port.version;
    for (const d of image.drivers) {
      if (!versions.has(d.name)) continue;
      d.version = versions.get(d.name);
      const drv = drivers.get(d.name);
      const v = drv && drv.versions.find((x) => x.version === d.version);
      if (v && !v.bus.includes(d.via)) { d.via = defaultVia(v); d.bus = defaultBus(d.via, d); }
    }
    update();
    announce('The selection now uses the upgraded versions.');
  }

  async function openFiles(list) {
    const status = $('open-status');
    const files = [];
    for (const f of [...list]) {
      if (f.size > 256 * 1024) { status.textContent = `${f.name} is larger than 256 KiB; is it a kb-image.yml?`; return; }
      files.push({ name: f.name, text: await f.text() });
    }
    const found = KB.project.classify(files);
    if (!found.image && !found.lock) {
      opened = null;
      renderUpgrade();
      status.textContent = found.problems.join(' ') || 'No file chosen.';
      return;
    }
    const lock = found.lock ? found.lock.doc : null;
    const img = found.image ? found.image.doc : KB.project.imageFromLock(lock);
    opened = { image: img, lock, label: [found.image && found.image.name, found.lock && found.lock.name].filter(Boolean).join(' + ') };
    targets = {};
    lastGood = null;
    const { selection, notes } = KB.project.toSelection(catalog, img, lock);
    if (hasRc(selection)) setShowRc(true, false);
    image = fillSelection(selection);
    update();
    const parts = [`Opened ${opened.label}${found.lock ? '' : ' (no lock: unnamed versions are today\'s defaults)'}.`, ...notes, ...found.problems];
    status.textContent = parts.join(' ');
    announce(parts[0]);
  }

  // -- the loop -----------------------------------------------------------------------------------------------
  function update(platformChanged) {
    normalize();
    if (platformChanged) {
      for (const d of image.drivers) {
        const drv = drivers.get(d.name);
        const v = drv && drv.versions.find((x) => x.version === d.version);
        if (v) { d.via = v.bus.includes(d.via) && kinds[d.via] && busCount(kinds[d.via].instance) > 0 ? d.via : defaultVia(v); d.bus = defaultBus(d.via, d); }
      }
      normalize();
    }
    result = KB.shared.run(catalog, image);
    output = generate(catalog, image, result);
    keepFocus(() => {
      renderFamilies();
      renderTargets();
      renderRtos();
      renderVersions();
      updateCards();
      renderAssign();
      renderBuses();
      renderFindings();
      renderOutputs();
      renderUpgrade();
    });
    // While an opened project is shown, its own image name stays out of the address bar (and the share link).
    const hash = encode(image, { rc: showRc, omitName: opened !== null });
    if (g.location.hash !== hash) g.history.replaceState(null, '', hash);
  }

  // One switch for release candidates: the catalogue, the version lists and the upgrade planner (--allow-rc).
  function setShowRc(on, refresh) {
    showRc = !!on;
    $('show-rc').checked = showRc;
    $('allow-rc').checked = showRc;
    if (refresh) {
      update();
      announce(showRc ? 'Release candidates shown.' : 'Release candidates hidden.');
    }
  }

  // -- theme ---------------------------------------------------------------------------------------------------
  function setTheme(choice) {
    const root = doc.documentElement;
    if (choice === 'light' || choice === 'dark') root.dataset.theme = choice; else delete root.dataset.theme;
    for (const b of doc.querySelectorAll('[data-theme-choice]')) b.setAttribute('aria-pressed', String(b.dataset.themeChoice === (choice || 'system')));
    try { g.localStorage.setItem('kb-configurator-theme', choice || 'system'); } catch (e) { /* storage blocked: fine */ }
  }

  function init() {
    const notice = $('file-notice');
    if (notice) notice.remove();
    let stored = 'system';
    try { stored = g.localStorage.getItem('kb-configurator-theme') || 'system'; } catch (e) { /* storage blocked */ }
    setTheme(stored);
    for (const b of doc.querySelectorAll('[data-theme-choice]')) b.addEventListener('click', () => setTheme(b.dataset.themeChoice));
    doc.body.append(h('div', { id: 'announce', class: 'visually-hidden', role: 'status', 'aria-live': 'polite' }));
    $('target').addEventListener('change', (e) => { image.mcu.target = e.target.value; update(true); });
    $('board').addEventListener('change', (e) => { image.mcu.board = e.target.value; update(); });
    $('kb-version').addEventListener('change', (e) => { image.kb_core = e.target.value || null; update(); });
    $('port-version').addEventListener('change', (e) => { image.port = e.target.value || null; update(); });
    $('image-name').addEventListener('change', (e) => { image.image = e.target.value; update(); });
    $('cxx').addEventListener('change', (e) => { image.cxx = e.target.value ? Number(e.target.value) : null; update(); });
    $('tabs').addEventListener('keydown', onTabKey);
    $('open-files').addEventListener('change', (e) => { openFiles(e.target.files).catch((err) => { $('open-status').textContent = `Could not read the files: ${err.message}`; }); });
    $('allow-rc').addEventListener('change', (e) => setShowRc(e.target.checked, true));
    $('show-rc').addEventListener('change', (e) => setShowRc(e.target.checked, true));
    $('upgrade-clear').addEventListener('click', () => { targets = {}; renderUpgrade(); announce('Chosen versions cleared.'); });
    $('upgrade-apply').addEventListener('click', applyUpgrade);
    $('upgrade-report').addEventListener('click', () => upgradeFile(KB.engine.REPORT_FILE));
    $('upgrade-image').addEventListener('click', () => upgradeFile(KB.engine.IMAGE_FILE));
    $('upgrade-lock').addEventListener('click', () => upgradeFile(KB.engine.LOCK_FILE));
    $('zip').addEventListener('click', () => {
      const bytes = zipProject(g.fflate, output);
      download(`${KB.core.safeName(image.image)}.zip`, bytes, 'application/zip');
      announce('Starter project downloaded.');
    });
    $('share').addEventListener('click', () => copy(g.location.href, 'Share link'));
    $('reset').addEventListener('click', () => {
      opened = null;
      targets = {};
      lastGood = null;
      setShowRc(false, false);
      $('open-files').value = '';
      $('open-status').textContent = '';
      image = defaults();
      update();
      announce('Selection cleared.');
    });
    $('figure-close').addEventListener('click', () => $('figure-dialog').close());
    $('figure-dialog').addEventListener('close', (e) => { const t = e.currentTarget.returnFocus; if (t) t.focus(); });
    g.addEventListener('hashchange', () => {
      const next = decode(g.location.hash);
      if (next) { image = next; setShowRc(flags(g.location.hash).rc || hasRc(next), false); update(); }
    });
    const fresh = catalog.source_date.slice(0, 10);
    $('catalog-meta').textContent = `Catalogue built ${fresh} from ${catalog.drivers.length} drivers; configurator ${KB.generate.SITE_VERSION}.`;
    buildCatalogue();
    setShowRc(showRc, false);
    update(false);
    doc.body.classList.remove('booting');
  }

  // The page renders at once with the site checks; js/boot.mjs adds the shared resolver and asks for a refresh.
  KB.ui = { refresh: () => update(false), openFiles };
  init();
})(globalThis);
