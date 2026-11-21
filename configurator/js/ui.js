// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The page: reads the catalogue (data/catalog.js), keeps the selection in the URL hash, runs the resolver on every
// change and shows the files and, in the supply-chain panel (js/panel.js), what the selection pulls in; opens an existing project's kb-image.yml and lock (read locally)
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
  const P = KB.picker;

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

  // The version a new selection starts from, whatever the release-candidate switch says: the newest release, else
  // the newest release candidate (the switch then turns on by itself), else a development revision. Before kb-core
  // has a final release the page would otherwise open with an error and nothing to build on.
  function startVersion(all) {
    for (const kind of ['release', 'rc', 'dev']) {
      const pool = (all || []).filter((r) => r.kind === kind);
      if (pool.length) return pool.slice().sort((a, b) => KB.core.compareVersions(b.version, a.version))[0].version;
    }
    return null;
  }

  function defaults() {
    const fam = catalog.families.find((f) => f.status === 'supported' && f.port) || catalog.families[0]; // the first family with a port
    const board = fam.boards.find((b) => b.usable_gpio !== null) || fam.boards[0];
    return {
      image: 'my-image',
      kb_core: startVersion(catalog.kb_core.releases),
      port: fam.port ? startVersion(fam.port.releases) : null,
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

  // -- step 1: target ------------------------------------------------------------------------------------------
  function renderFamilies() {
    const box = $('families');
    box.replaceChildren(box.querySelector('legend') || h('legend', { text: 'MCU family' }));
    for (const fam of catalog.families) {
      const id = 'fam-' + fam.id;
      box.append(h('div', { class: 'choice' },
        h('input', { type: 'radio', name: 'family', id, value: fam.id, checked: image.mcu.family === fam.id,
          'aria-describedby': id + '-d',
          onchange: () => change(() => { image.mcu.family = fam.id; image.mcu.target = ''; image.port = fam.port ? newestVersion(fam.port.releases) : null; }, true) }),
        h('label', { class: 'face', for: id },
          h('strong', {}, fam.label, ' ', fam.status === 'planned' ? h('span', { class: 'badge planned', text: 'planned' }) : null),
          h('span', { id: id + '-d', text: fam.port ? `${fam.targets.length} target${fam.targets.length === 1 ? '' : 's'}` : 'No port yet' }))));
    }
  }

  function renderTargets() {
    const fam = families.get(image.mcu.family);
    $('target').replaceChildren(...fam.targets.map((t) => h('option', { value: t.id, selected: t.id === image.mcu.target, text: t.label })));
    const target = fam.targets.find((t) => t.id === image.mcu.target);
    $('target-hint').textContent = target
      ? ['i2c', 'spi', 'uart', 'can', 'pio'].filter((k) => target.buses[k]).map((k) => `${target.buses[k]} ${k === 'pio' ? 'RMT' : k.toUpperCase()}`).join(', ')
      : '';
  }

  // Only the backends the family allows are offered (catalogue families[].rtos).
  function renderRtos() {
    const fam = families.get(image.mcu.family);
    const box = $('rtos');
    box.replaceChildren(box.querySelector('legend') || h('legend', { text: 'RTOS' }));
    box.title = fam.rtos_note || '';
    for (const r of P.rtosFor(catalog, fam.id)) {
      const id = 'rtos-' + r.id;
      box.append(h('div', { class: 'choice' },
        h('input', { type: 'radio', name: 'rtos', id, value: r.id, checked: image.rtos === r.id,
          'aria-describedby': id + '-d', onchange: () => change(() => { image.rtos = r.id; }) }),
        h('label', { class: 'face', for: id },
          h('strong', {}, r.label.replace(/ \(.*$/, ''), ' ', r.status === 'planned' ? h('span', { class: 'badge planned', text: 'planned' }) : null),
          h('span', { id: id + '-d', text: r.status === 'planned' ? 'Not released yet' : r.id === fam.default_rtos ? 'Default' : '' }))));
    }
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
    const def = P.defaultStandard(catalog, image.mcu.family);
    const explicitDefault = image.cxx === def;
    $('cxx').replaceChildren(
      h('option', { value: '', selected: !image.cxx, text: `C++${def} (default)` }),
      ...(explicitDefault ? [h('option', { value: String(def), selected: true, text: `C++${def} (declared)` })] : []),
      ...[20, 23].filter((n) => n !== def).map((n) => h('option', { value: String(n), selected: image.cxx === n, text: `C++${n}` })));
  }

  // -- step 2: the driver picker -------------------------------------------------------------------------------
  const VIEW_KEY = 'kb-configurator-view';
  let view = P.defaults();
  let shown = P.CHUNK; // how many of a long result are rendered; "Show more" adds a chunk
  let unavail = new Map(); // driver name to {code, reason}, from KB.engine.availability for the current selection
  const hay = new Map();
  const nodes = new Map(); // name to {card, row, add, rowAdd, ...}; built once, repainted on every change
  let cascadeUndo = null; // {image, text}: the selection before the change that dropped something

  function storedView() {
    try { const m = g.localStorage.getItem(VIEW_KEY); return P.VIEWS.includes(m) ? m : null; } catch (e) { return null; }
  }

  function selected(name) {
    return image.drivers.find((d) => d.name === name) || null;
  }

  function openFigures(d, trigger) {
    const dlg = $('figure-dialog');
    $('figure-title').textContent = `${d.chip}: figures`;
    $('figure-body').replaceChildren(...d.figures.map((f) => h('figure', {},
      h('img', { class: 'fig-light', src: f.light, alt: f.alt, loading: 'lazy', decoding: 'async' }),
      h('img', { class: 'fig-dark', src: f.dark, alt: f.alt, loading: 'lazy', decoding: 'async' }),
      h('figcaption', { text: `${f.kind[0].toUpperCase()}${f.kind.slice(1)}. From the docs of ${d.repo.split('/')[1]}.` }))),
    d.capabilities.length ? h('dl', { class: 'facts' }, d.capabilities.flatMap((c) => [h('dt', { text: c.label }), h('dd', { text: c.text })])) : null);
    dlg.returnFocus = trigger;
    dlg.showModal();
  }

  const busBadges = (d) => P.busesOf(d).map((b) => h('li', { class: 'badge bus', text: P.busLabel(kinds, b) }));
  const catBadge = (d) => h('li', { class: 'badge cat', text: P.categoryOf(d).replace('-', ' ') });

  function makeNode(d) {
    const titleId = 'dn-' + d.name;
    const fig = d.figures[0];
    const thumb = fig
      ? h('button', { type: 'button', class: 'thumb', 'aria-label': `Show the figures of ${d.chip}`, onclick: (e) => openFigures(d, e.currentTarget) },
        h('img', { class: 'fig-light', src: fig.light, alt: '', loading: 'lazy', decoding: 'async', width: 240, height: 134 }),
        h('img', { class: 'fig-dark', src: fig.dark, alt: '', loading: 'lazy', decoding: 'async', width: 240, height: 134 }))
      : h('div', { class: 'thumb empty', text: 'No figure published' });
    const mkAdd = (id) => h('button', { type: 'button', class: 'add', id, 'aria-pressed': 'false', onclick: () => toggleDriver(d) });
    const add = mkAdd('add-' + d.name);
    const note = h('p', { class: 'sub reason', hidden: true });
    const status = d.status === 'unpublished' ? h('span', { class: 'badge unpublished', text: 'not published' }) : null;
    const purpose = d.summary || 'No public release yet, so it cannot be pinned.';
    const card = h('li', { class: 'card', id: 'card-' + d.name, 'aria-labelledby': titleId },
      thumb,
      h('div', { class: 'card-body' },
        h('h3', { id: titleId, title: d.name }, d.chip, status),
        h('p', { class: 'summary', title: purpose, text: purpose }),
        h('ul', { class: 'tags', 'aria-label': 'Bus and category' }, busBadges(d), catBadge(d)),
        note,
        h('div', { class: 'actions' }, add)));
    const rowAdd = mkAdd('add-' + d.name);
    const rowNote = h('span', { class: 'sub reason', hidden: true });
    const row = h('tr', { class: 'row-item', id: 'row-' + d.name },
      h('th', { scope: 'row' }, h('strong', { text: d.chip }), ' ', h('span', { class: 'sub', text: d.name }), rowNote),
      h('td', { class: 'purpose', title: purpose, text: purpose }),
      h('td', {}, h('ul', { class: 'tags', 'aria-label': 'Buses' }, busBadges(d))),
      h('td', { text: P.categoryOf(d).replace('-', ' ') }),
      h('td', {}, rowAdd));
    return { d, card, row, add, rowAdd, note, rowNote };
  }

  // The state of a driver's card and row: added or not, unavailable (with the reason), release candidates only.
  function paintNode(n) {
    const { d } = n;
    const s = selected(d.name);
    const u = unavail.get(d.name);
    const noVersion = !visibleVersions(d.versions, s ? s.version : null).length;
    const blocked = !s && (!!u || noVersion);
    const reason = u && !s ? u.reason : noVersion && !s && d.versions.length ? 'Release candidates only: show them in Advanced.' : '';
    for (const [el, noteEl] of [[n.add, n.note], [n.rowAdd, n.rowNote]]) {
      el.disabled = blocked || (!d.versions.length && !s);
      el.setAttribute('aria-pressed', String(!!s));
      el.replaceChildren(s ? 'Remove' : 'Add', h('span', { class: 'visually-hidden', text: ` ${d.chip}` }));
      noteEl.hidden = !reason;
      noteEl.textContent = reason;
    }
    n.card.dataset.selected = String(!!s);
    n.row.dataset.selected = String(!!s);
    n.card.dataset.unavailable = String(!!u);
  }

  function nodeFor(d) {
    if (!nodes.has(d.name)) nodes.set(d.name, makeNode(d));
    return nodes.get(d.name);
  }

  function itemsOf(list, asRows) {
    return list.map((d) => { const n = nodeFor(d); paintNode(n); return asRows ? n.row : n.card; });
  }

  function listTable(list) {
    const sortBtn = (key, label) => {
      const on = view.sort === key;
      return h('th', { scope: 'col', 'aria-sort': on ? (view.desc ? 'descending' : 'ascending') : 'none' },
        h('button', { type: 'button', class: 'ghost sort-head', id: 'sort-h-' + key, onclick: () => {
          if (view.sort === key) view.desc = !view.desc; else { view.sort = key; view.desc = false; }
          applyView();
        } }, label, on ? (view.desc ? ' ↓' : ' ↑') : ''));
    };
    return h('div', { class: 'table-wrap' }, h('table', { class: 'list' },
      h('caption', { class: 'visually-hidden', text: 'Drivers' }),
      h('thead', {}, h('tr', {}, sortBtn('name', 'Driver'), h('th', { scope: 'col', text: 'Purpose' }), sortBtn('bus', 'Bus'),
        sortBtn('category', 'Category'), h('th', { scope: 'col' }, h('span', { class: 'visually-hidden', text: 'Action' })))),
      h('tbody', {}, itemsOf(list, true))));
  }

  const gridOf = (list, label) => h('ul', { class: 'grid', 'aria-label': label }, itemsOf(list, false));

  function paintList(box, groups, label) {
    const asList = view.view === 'list';
    if (!groups.length) { box.replaceChildren(); return; }
    if (view.view === 'grouped') {
      box.replaceChildren(...groups.map((gr) => h('section', { class: 'group', 'aria-label': gr.label },
        h('div', { class: 'group-title', 'aria-hidden': 'true' }, gr.label.replace('-', ' '), h('span', { class: 'sub', text: ` ${gr.items.length}` })),
        gridOf(gr.items, gr.label))));
    } else {
      const items = groups.flatMap((gr) => gr.items);
      box.replaceChildren(asList ? listTable(items) : gridOf(items, label));
    }
  }

  function renderCatalogue() {
    unavail = P.unavailableMap(catalog, image);
    const res = P.filter(catalog.drivers, view, unavail, kinds, hay);
    const ordered = P.sort(res.items, view.sort, view.desc, kinds);
    const groups = view.view === 'grouped' ? P.group(ordered, view.group, kinds) : [{ key: '', label: '', items: ordered }];
    const cap = P.limit(ordered.length, shown);
    paintList($('catalogue'), P.take(groups, cap), 'Driver catalogue');
    $('more').hidden = cap >= ordered.length;
    if (!$('more').hidden) $('more').textContent = `Show more (${ordered.length - cap} left)`;
    const total = catalog.drivers.length;
    const text = `${ordered.length} of ${total} drivers shown, ${image.drivers.length} selected.`;
    if ($('count').textContent !== text) $('count').textContent = text;
    // Drivers this target cannot use: listed only on request, each with the one-line reason.
    const un = P.sort(res.unavailable, view.sort, view.desc, kinds);
    $('unavail-box').hidden = !un.length;
    $('unavail-toggle').textContent = `${view.unavail ? 'Hide' : 'Show'} ${un.length} unavailable`;
    $('unavail-toggle').setAttribute('aria-expanded', String(view.unavail));
    $('unavail').hidden = !view.unavail || !un.length;
    if (view.unavail && un.length) paintList($('unavail'), [{ key: '', label: '', items: un }], 'Unavailable drivers');
    else $('unavail').replaceChildren();
    // No match for a search: offer the intake form, prefilled with the search text reduced to a part-number alphabet.
    const ask = $('request-chip');
    const part = view.q.replace(/[^A-Za-z0-9._-]/g, '').slice(0, 40);
    ask.hidden = ordered.length > 0 || !part;
    if (!ask.hidden) {
      ask.replaceChildren(`No driver for ${part}: `, h('a', {
        href: `${REQUESTS}?template=request-driver.yml&part=${encodeURIComponent(part)}`, target: '_blank', rel: 'noopener noreferrer',
        text: 'request it' }), ' (opens the request form on GitHub).');
    }
    renderTray();
  }

  const REQUESTS = 'https://github.com/kokeb-labs-drivers/driver-requests/issues/new';

  // The chosen drivers, with a one-line hint when two share a bus.
  function renderTray() {
    const box = $('tray');
    if (!image.drivers.length) { box.replaceChildren(h('p', { class: 'sub', text: 'No drivers chosen yet.' })); return; }
    const chips = image.drivers.map((s) => {
      const d = drivers.get(s.name);
      const u = unavail.get(s.name);
      return h('li', { class: 'tray-chip' + (u ? ' bad' : ''), title: u ? u.reason : `${s.name} on ${s.bus}` },
        h('strong', { text: d ? d.chip : s.name }), h('span', { class: 'sub', text: s.bus }),
        u ? h('span', { class: 'badge warn', text: 'unavailable' }) : null,
        h('button', { type: 'button', class: 'ghost', id: 'tray-rm-' + s.name, 'aria-label': `Remove ${d ? d.chip : s.name}`, text: '×',
          onclick: () => toggleDriver(d || { name: s.name, chip: s.name }) }));
    });
    const shared = P.sharedBuses(image.drivers, kinds);
    box.replaceChildren(h('strong', { class: 'tray-title', text: `Selected (${image.drivers.length})` }), h('ul', { class: 'tray-list' }, chips),
      shared.length ? h('p', { class: 'sub shared-hint', text: shared.map((x) => `${x.names.join(' and ')} share ${x.bus}.`).join(' ') }) : null);
  }

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
      if (d.requires_owner && image.buses[entry.bus]) image.buses[entry.bus] = { model: 'owner' }; // the bus must be an owner bus
      image.drivers.sort((a, b) => (a.name < b.name ? -1 : 1));
      announce(`${d.chip} added on ${entry.bus}.`);
    }
    clearCascade();
    update();
  }

  // -- filters, views and the address bar ------------------------------------------------------------------------
  function chip(group, id, label, count) {
    return h('button', { type: 'button', 'aria-pressed': 'false', id: `f-${group}-${id}`, 'data-group': group, 'data-id': id,
      onclick: () => {
        const list = view[group];
        view[group] = list.includes(id) ? list.filter((x) => x !== id) : [...list, id].sort();
        applyView(true);
      } }, label, count !== undefined ? h('span', { class: 'n', text: String(count) }) : null);
  }

  function buildFilters() {
    const busCount = new Map();
    for (const d of catalog.drivers) for (const b of P.busesOf(d)) busCount.set(b, (busCount.get(b) || 0) + 1);
    $('bus-filters').replaceChildren(...[...busCount.keys()].sort().map((b) => chip('bus', b, P.busLabel(kinds, b), busCount.get(b))));
    const idx = catalog.tag_index || { categories: {}, tags: {} };
    $('cat-filters').replaceChildren(...Object.keys(idx.categories).filter((c) => idx.categories[c] > 0).sort()
      .map((c) => chip('cat', c, c.replace('-', ' '), idx.categories[c])));
    $('tag-filters').replaceChildren(...Object.keys(idx.tags).sort().map((t) => chip('tag', t, t, idx.tags[t])));
  }

  function syncControls() {
    for (const b of doc.querySelectorAll('.filters button')) b.setAttribute('aria-pressed', String(view[b.dataset.group].includes(b.dataset.id)));
    for (const b of doc.querySelectorAll('[data-view]')) b.setAttribute('aria-pressed', String(b.dataset.view === view.view));
    $('group-by').value = view.group;
    $('group-by').disabled = view.view !== 'grouped';
    $('sort-by').value = view.sort;
    $('sort-dir').textContent = view.desc ? 'Z to A' : 'A to Z';
    $('sort-dir').setAttribute('aria-pressed', String(view.desc));
    $('fit').checked = view.fit;
    if ($('search').value !== view.q && doc.activeElement !== $('search')) $('search').value = view.q;
    $('tag-active').textContent = view.tag.length ? `(${view.tag.length} chosen)` : '';
    if (view.tag.length) $('tag-details').open = true;
    $('filters-reset').disabled = !P.filtered(view) && view.sort === 'name' && !view.desc;
  }

  // Re-render the list after a filter, sort, group or view change, and write the new view to the address bar.
  function applyView(resetChunk) {
    if (resetChunk !== false) shown = P.CHUNK;
    keepFocus(() => { renderCatalogue(); syncControls(); });
    syncHash();
  }

  function setViewMode(mode) {
    view.view = mode;
    try { g.localStorage.setItem(VIEW_KEY, mode); } catch (e) { /* storage blocked: the choice lasts for this visit */ }
    applyView();
    announce(`${{ grid: 'Card', list: 'List', grouped: 'Grouped' }[mode]} view.`);
  }

  function syncHash() {
    // While an opened project is shown, its own image name stays out of the address bar (and the share link).
    const hash = encode(image, { rc: showRc, omitName: KB.upgrade.opened() !== null, view });
    if (g.location.hash !== hash) g.history.replaceState(null, '', hash);
  }

  // -- the cascade: what a change to the target, RTOS or C++ standard takes away ---------------------------------
  function clearCascade() {
    cascadeUndo = null;
    $('cascade').hidden = true;
  }

  // Apply a change to the target, the RTOS or the standard; keep what still fits and say what dropped, with Undo.
  function change(mutate, platform) {
    const before = P.clone(image);
    mutate();
    normalize();
    const drops = P.cascade(before, image, P.unavailableMap(catalog, image), catalog);
    image.drivers = drops.kept;
    normalize();
    const text = P.describe(drops);
    if (text) {
      cascadeUndo = { image: before, text };
      $('cascade-text').textContent = text + ' ';
      $('cascade').hidden = false;
      announce(text);
    } else {
      clearCascade();
    }
    update(platform);
  }

  function undoCascade() {
    if (!cascadeUndo) return;
    image = cascadeUndo.image;
    clearCascade();
    update(true);
    announce('Change undone.');
  }

  // -- advanced: versions, buses, handlers, thread models --------------------------------------------------------------
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
      box.replaceChildren(h('p', { class: 'empty-note', text: 'Add drivers in step 2; each one appears here.' }));
      return;
    }
    const rows = image.drivers.map((s) => {
      const d = drivers.get(s.name);
      const v = d && d.versions.find((x) => x.version === s.version);
      const vias = v ? v.bus : [s.via];
      return h('tr', {},
        h('td', {}, h('strong', { text: d ? d.chip : s.name }), ' ', h('span', { class: 'sub', text: s.name })),
        h('td', {}, h('label', { class: 'visually-hidden', for: 'ver-' + s.name, text: `Version of ${s.name}` }),
          h('select', { id: 'ver-' + s.name, onchange: (e) => {
            s.version = e.target.value;
            const nv = d && d.versions.find((x) => x.version === s.version);
            if (nv && !nv.bus.includes(s.via)) { s.via = defaultVia(nv); s.bus = defaultBus(s.via, s); }
            update();
          } }, d ? visibleVersions(d.versions, s.version).map((x) => h('option', { value: x.version, selected: x.version === s.version, text: verText(x) }))
            : h('option', { value: s.version, text: s.version }))),
        h('td', {}, h('label', { class: 'visually-hidden', for: 'via-' + s.name, text: `Bus kind of ${s.name}` }),
          h('select', { id: 'via-' + s.name, disabled: vias.length < 2, onchange: (e) => { s.via = e.target.value; s.bus = defaultBus(s.via, s); update(); } },
            vias.map((b) => h('option', { value: b, selected: b === s.via, text: (kinds[b] && kinds[b].label) || b })))),
        h('td', {}, h('label', { class: 'visually-hidden', for: 'bus-' + s.name, text: `Bus of ${s.name}` }),
          h('select', { id: 'bus-' + s.name, onchange: (e) => { s.bus = e.target.value; update(); } }, busOptions(s.via, s.bus))),
        h('td', {}, d && d.requires_owner ? h('span', { class: 'badge muted', text: 'owner task only' }) : h('span', { class: 'sub', text: 'either model' })));
    });
    box.replaceChildren(h('div', { class: 'table-wrap' }, h('table', { class: 'assign' },
      h('caption', { class: 'visually-hidden', text: 'Bus of each selected driver' }),
      h('thead', {}, h('tr', {}, ['Driver', 'Version', 'Talks over', 'Bus', 'Handler'].map((t) => h('th', { scope: 'col', text: t })))),
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
  }

  // -- result: the small-screen bar; the panel itself is js/panel.js ---------------------------------------------
  function renderFindings() {
    const errors = result.findings.filter((f) => f.severity === 'error').length;
    const warnings = result.findings.filter((f) => f.severity === 'warning').length;
    $('bar-status').textContent = errors ? `${errors} blocker${errors === 1 ? '' : 's'} to fix` : `Ready, ${warnings} warning${warnings === 1 ? '' : 's'}`;
    $('bar-status').className = errors ? 'bar-status err' : 'bar-status ok';
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
    $('bar-zip').disabled = !output.ready;
    $('zip-note').textContent = output.ready
      ? `Holds every file above plus a project skeleton; built in your browser, repeatable byte for byte.`
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
      renderCatalogue();
      syncControls();
      renderAssign();
      renderBuses();
      renderFindings();
      renderOutputs();
      KB.upgrade.render();
      if (KB.panel) KB.panel.render({ catalog, image, result, output, h, download, copy, announce, zip: () => $('zip').click() });
      if (KB.views) KB.views.render(catalog, image, result.shared && result.shared.ok ? result.shared.lock : null);
    });
    syncHash();
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
    $('target').addEventListener('change', (e) => change(() => { image.mcu.target = e.target.value; }, true));
    $('kb-version').addEventListener('change', (e) => { image.kb_core = e.target.value || null; update(); });
    $('port-version').addEventListener('change', (e) => { image.port = e.target.value || null; update(); });
    $('image-name').addEventListener('change', (e) => { image.image = e.target.value; update(); });
    $('cxx').addEventListener('change', (e) => change(() => { image.cxx = e.target.value ? Number(e.target.value) : null; }));
    $('tabs').addEventListener('keydown', onTabKey);
    $('allow-rc').addEventListener('change', (e) => setShowRc(e.target.checked, true));
    $('show-rc').addEventListener('change', (e) => setShowRc(e.target.checked, true));
    $('zip').addEventListener('click', () => {
      const bytes = zipProject(g.fflate, output);
      download(`${KB.core.safeName(image.image)}.zip`, bytes, 'application/zip');
      announce('Starter project downloaded.');
    });
    $('bar-zip').addEventListener('click', () => $('zip').click());
    for (const id of ['share', 'share-out']) $(id).addEventListener('click', () => copy(g.location.href, 'Share link'));
    $('search').addEventListener('input', (e) => { view.q = P.sanitize({ q: e.target.value }).q; applyView(); });
    for (const b of doc.querySelectorAll('[data-view]')) b.addEventListener('click', () => setViewMode(b.dataset.view));
    $('group-by').addEventListener('change', (e) => { view.group = e.target.value; applyView(); });
    $('sort-by').addEventListener('change', (e) => { view.sort = e.target.value; applyView(); });
    $('sort-dir').addEventListener('click', () => { view.desc = !view.desc; applyView(); });
    $('fit').addEventListener('change', (e) => { view.fit = e.target.checked; applyView(); });
    $('filters-reset').addEventListener('click', () => { view = P.reset(view); $('search').value = ''; applyView(); announce('Filters cleared.'); });
    $('more').addEventListener('click', () => { shown += P.CHUNK; applyView(false); announce('More drivers shown.'); });
    $('unavail-toggle').addEventListener('click', () => { view.unavail = !view.unavail; applyView(false); });
    $('cascade-undo').addEventListener('click', undoCascade);
    $('reset').addEventListener('click', () => {
      KB.upgrade.reset();
      setShowRc(false, false);
      image = defaults();
      view = P.reset(view);
      $('search').value = '';
      clearCascade();
      if (hasRc(image)) setShowRc(true, false);
      update();
      announce('Selection cleared.');
    });
    $('figure-close').addEventListener('click', () => $('figure-dialog').close());
    $('figure-dialog').addEventListener('close', (e) => { const t = e.currentTarget.returnFocus; if (t) t.focus(); });
    g.addEventListener('hashchange', () => {
      const next = decode(g.location.hash);
      if (next) {
        image = next;
        view = KB.state.viewState(g.location.hash) || view;
        clearCascade();
        setShowRc(flags(g.location.hash).rc || hasRc(next), false);
        update();
      }
    });
    const fresh = catalog.source_date.slice(0, 10);
    $('catalog-meta').textContent = `Catalogue built ${fresh} from ${catalog.drivers.length} drivers; configurator ${KB.generate.SITE_VERSION}.`;
    KB.upgrade.init({ $, h, announce, download, defaultVia, defaultBus, pickVersion, visibleVersions, image: () => image,
      setImage: (x) => { image = x; }, hasRc, setShowRc, showRc: () => showRc, update });
    view = KB.state.viewState(g.location.hash) || ((m) => (m ? { ...view, view: m } : view))(storedView());
    buildFilters();
    setShowRc(showRc, false);
    update(false);
    doc.body.classList.remove('booting');
  }

  // The page renders at once with the site checks; js/boot.mjs adds the shared resolver and asks for a refresh.
  KB.ui = { refresh: () => update(false) };
  init();
})(globalThis);
