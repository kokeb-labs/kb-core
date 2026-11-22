// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// "Open an existing project" (Advanced): reads a kb-image.yml and its lock in the browser, shows the upgrades the shared
// planner offers (the rules of kb upgrade) and applies them to the selection. Split from ui.js, which hands it the page's
// helpers through KB.upgrade.init(ui). The files stay in memory only, never in the address bar.
(function (g) {
  'use strict';
  const KB = g.KB;
  const catalog = g.KB_CATALOG;
  const { latestVersion } = KB.resolver;
  const drivers = new Map(catalog.drivers.map((d) => [d.name, d]));
  let ui = null;
  let $, h, announce, download, defaultVia, defaultBus, pickVersion, visibleVersions;
  const showRc = () => ui.showRc();
  const update = () => ui.update();

  // -- open an existing project and its upgrades ---------------------------------------------------------------
  // The files stay in memory only; the plan is the shared planner's (resolver.mjs plan, the rules of kb upgrade).
  let opened = null;
  let targets = {};
  let upgradePlan = null;
  let lastGood = null; // the last plan that worked: its rows stay on screen while a chosen version is refused

  function allowRc() {
    return showRc();
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
        .filter((v) => showRc() || v.kind !== 'rc' || v.version === r.from || v.version === targets[r.name])
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
    const image = ui.image();
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
    if (ui.hasRc(selection)) ui.setShowRc(true, false);
    ui.setImage(fillSelection(selection));
    update();
    const parts = [`Opened ${opened.label}${found.lock ? '' : ' (no lock: unnamed versions are today\'s defaults)'}.`, ...notes, ...found.problems];
    status.textContent = parts.join(' ');
    announce(parts[0]);
  }

  function init(api) {
    ui = api;
    ({ $, h, announce, download, defaultVia, defaultBus, pickVersion, visibleVersions } = api);
    $('open-files').addEventListener('change', (e) => { openFiles(e.target.files).catch((err) => { $('open-status').textContent = `Could not read the files: ${err.message}`; }); });
    $('upgrade-clear').addEventListener('click', () => { targets = {}; renderUpgrade(); announce('Chosen versions cleared.'); });
    $('upgrade-apply').addEventListener('click', applyUpgrade);
    $('upgrade-report').addEventListener('click', () => upgradeFile(KB.engine.REPORT_FILE));
    $('upgrade-image').addEventListener('click', () => upgradeFile(KB.engine.IMAGE_FILE));
    $('upgrade-lock').addEventListener('click', () => upgradeFile(KB.engine.LOCK_FILE));
  }

  function reset() {
    opened = null;
    targets = {};
    lastGood = null;
    $('open-files').value = '';
    $('open-status').textContent = '';
  }

  KB.upgrade = { init, render: renderUpgrade, reset, opened: () => opened, openFiles };
})(globalThis);
