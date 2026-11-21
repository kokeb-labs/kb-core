// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The right-hand panel: what the selection pulls in and what is known about it, not a wall of combinations.
//   SBOM        SPDX 2.3 and CycloneDX 1.6 downloads, component count, licences, pinned versions
//   CVE         the weekly scan (data/vulnerabilities.js, kb-vulns/1) for the selected components only; "none known"
//               only with data, "not scanned" for a component the scan does not hold, a warning past 10 days; an
//               advisory with a `review` (tools/vuln/reviewed.json) is "fixed in the pinned version" and not open
//   Provenance  the release asset of the selected kb-core version and the command that verifies its attestation
//   Safety      tests, coverage and MISRA as three chips per driver, from kb-core's evidence until a driver has its own
//   Blockers    only the resolver's errors, next to the cause; warnings and notes sit in a closed "Details"
// Loaded after the first view (defer); no network, no inline style. The model functions are pure (KB.panel.model) so
// node --test checks them; render() only builds DOM.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const DOCS = 'https://kokeb-labs.github.io/kb-core/latest/';
  const STALE_DAYS = 10;
  const DAY = 86400000;
  const bare = (v) => String(v || '').replace(/^v/, '');
  const num = (v) => typeof v === 'number' && Number.isFinite(v);
  const plural = (n, w, ws) => `${n} ${n === 1 ? w : ws || w + 's'}`;

  // -- the model (pure) ---------------------------------------------------------------------------------------
  const dayOf = (s) => { const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(s || ''); return m ? Date.UTC(+m[1], +m[2] - 1, +m[3]) / DAY : null; };
  const utcDay = (now) => { const d = new Date(now); return Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()) / DAY; };

  // missing (no usable data), fresh, or stale: the scan is stale from the 11th day on (10 days old is still fresh).
  function freshness(data, now) {
    const checked = data && Array.isArray(data.components) ? dayOf(data.checked_at) : null;
    if (checked === null) return { state: 'missing', checked: null, age: null };
    const age = utcDay(now) - checked;
    return { state: age > STALE_DAYS ? 'stale' : 'fresh', checked: data.checked_at, age };
  }

  // Everything the selection pulls in: kb-core, the port, the drivers (output.rows) and the SDK and RTOS (output.soup).
  function components(output) {
    const rows = output.rows.map((r) => ({ name: r.name, label: r.c ? r.c.d.chip : r.name, version: r.version,
      kind: r.name === 'kb-core' ? 'kb-core' : r.name.startsWith('port-') ? 'port' : 'driver', licence: r.licence, pin: r.kind }));
    const soup = output.soup.map((s) => ({ name: s.name, label: s.name, version: s.version, kind: 'sdk', licence: s.licence, pin: null }));
    return rows.concat(soup);
  }

  const safeUrl = (u) => (/^https:\/\/[^\s"'<>]+$/.test(u || '') ? u : null);
  // A review is shown only when it says why and where from: a known status, a reason and an https source.
  const reviewOf = (r) => (r && (r.status === 'fixed_in_pinned_version' || r.status === 'not_affected') && r.reason && safeUrl(r.source)
    ? { status: r.status, reason: String(r.reason), source: r.source, reviewed: String(r.reviewed || '') } : null);
  const reviewWord = (r) => (r.status === 'not_affected' ? 'not affected' : 'fixed in the pinned version');

  // One entry per selected component: scanned (none known or advisories) or "not scanned". A pinned version is matched
  // without its leading v; the SDK and RTOS, which the lock does not pin, match by name.
  function cve(data, comps, now) {
    const fr = freshness(data, now);
    const items = comps.map((c) => {
      const e = fr.state === 'missing' ? null
        : data.components.find((x) => x.name === c.name && (!c.version || bare(x.version) === bare(c.version)));
      const base = { name: c.name, label: c.label, version: (e && e.version) || c.version };
      if (!e) return { ...base, state: 'not scanned', count: 0, open: 0, reviewed: 0, advisories: [] };
      const adv = (e.advisories || []).map((a) => ({ id: a.id, severity: a.severity, summary: a.summary, url: safeUrl(a.url), review: reviewOf(a.review) }));
      const count = Math.max(adv.length, num(e.count) ? e.count : 0);
      const reviewed = adv.filter((a) => a.review).length;
      const open = count - reviewed;
      return { ...base, state: open ? 'advisories' : reviewed ? 'reviewed' : 'none known', count, open, reviewed, advisories: adv };
    });
    const scanned = items.filter((i) => i.state !== 'not scanned').length;
    const advisories = items.reduce((a, i) => a + i.count, 0);
    const open = items.reduce((a, i) => a + i.open, 0);
    const reviewed = advisories - open;
    const status = fr.state === 'missing' ? 'no data' : open ? 'advisories found' : reviewed ? 'reviewed' : scanned ? 'none known' : 'not scanned';
    return { fresh: fr, items, scanned, notScanned: items.length - scanned, advisories, open, reviewed, status,
      stale: fr.state === 'stale', missing: fr.state === 'missing' };
  }

  function licences(comps) {
    const set = [...new Set(comps.map((c) => c.licence).filter(Boolean))].sort();
    return { list: set, text: set.length === 0 ? 'none' : set.length === 1 ? `all ${set[0]}` : set.join(', ') };
  }

  // Component count of the SBOM (the lock's components plus the SDK and RTOS) and how many versions are pinned.
  function sbom(output, lockComponents) {
    const comps = components(output);
    const pinned = output.rows.filter((r) => r.kind === 'release' || r.kind === 'rc');
    return { ready: !!output.lock, count: output.lock ? lockComponents(output.lock).length + output.soup.length : 0,
      licences: licences(comps), pinned: pinned.length, pins: output.rows.length,
      rc: output.rows.filter((r) => r.kind === 'rc').length, dev: output.rows.filter((r) => r.kind === 'dev').length };
  }

  // The selected kb-core release asset and the command that checks its attestation. The evidence holds no attestation
  // fact, so the state is "per release process" (docs/versioning.md, release.yml), never "verified".
  function provenance(ev, repo) {
    const assets = ev && ev.supply && Array.isArray(ev.supply.release_assets) ? ev.supply.release_assets : [];
    const asset = assets.find((a) => /\.tar\.gz$/.test(a.name)) || assets[0];
    if (!asset || !/^[A-Za-z0-9._-]+$/.test(asset.name) || !/^[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(repo || '')) {
      return { state: 'none', asset: null, command: null };
    }
    return { state: 'per release process', asset: asset.name, sha256: asset.sha256 || null, version: ev.release.version,
      command: `gh attestation verify ${asset.name} --repo ${repo}` };
  }

  // Tests, coverage and MISRA as chips from one evidence block. kb-core's evidence (kb-evidence/1) and a future
  // per-driver block (docs/assurance/evidence.md, "Per-driver evidence") have the same fields.
  function chips(block, links) {
    if (!block) return [];
    const out = [];
    const t = block.tests;
    if (t && num(t.total) && num(t.failures)) {
      out.push({ id: 'tests', text: `Tests ${t.total}`, title: `${t.total} tests, ${t.failures} failed`, tone: t.failures === 0 && t.total > 0 ? 'ok' : 'err', href: links.tests });
    }
    const c = block.coverage;
    if (c && c.branch && c.line && num(c.branch.percent) && num(c.line.percent)) {
      const floor = c.floors && num(c.floors.branch) ? c.floors.branch : null;
      out.push({ id: 'coverage', text: `Coverage ${c.branch.percent}%`, title: `Line ${c.line.percent} %, branch ${c.branch.percent} %`,
        tone: floor === null ? 'muted' : c.branch.percent >= floor ? 'ok' : 'err', href: links.docs });
    }
    const m = block.misra;
    if (m && typeof m.passed === 'boolean') {
      out.push({ id: 'misra', text: `MISRA ${m.passed ? 'pass' : 'fail'}`, title: `${m.edition || 'MISRA'}, ${m.passed ? 'passed' : 'not passed'}`, tone: m.passed ? 'ok' : 'err', href: links.docs });
    }
    return out;
  }

  // Per selected driver: its own evidence block when the data holds one (ev.drivers[name]), else kb-core's, labelled.
  function safety(ev, image) {
    const links = { tests: ev && ev.source ? safeUrl(ev.source.run_url) : null, docs: `${DOCS}assurance/evidence/` };
    return image.drivers.map((d) => {
      const own = ev && ev.drivers && ev.drivers[d.name];
      const block = own || ev;
      return { name: d.name, own: !!own, label: own ? 'driver evidence' : 'kb-core evidence',
        chips: chips(block, own && own.source ? { ...links, tests: safeUrl(own.source.run_url) } : links) };
    });
  }

  const blockers = (findings) => findings.filter((f) => f.severity === 'error');
  const details = (findings) => findings.filter((f) => f.severity !== 'error');

  // Where a finding's subject lives on the page: a driver, a bus, the target, the RTOS or a version.
  function causeOf(subject, image) {
    if (!subject) return null;
    if ((image.drivers || []).some((d) => d.name === subject)) return { kind: 'driver', id: subject };
    let m = /^buses\.([^.]+)/.exec(subject);
    if (m) return { kind: 'bus', id: m[1] };
    m = /^([^/]+)\/(.+)$/.exec(subject);
    if (m) return { kind: 'bus', id: m[1] };
    if (subject === 'adc' || subject === 'pins') return { kind: 'bus', id: null };
    if (subject === 'rtos') return { kind: 'rtos', id: null };
    if (subject.startsWith('mcu.port') || subject.startsWith('port-')) return { kind: 'version', id: 'port-version' };
    if (subject.startsWith('mcu')) return { kind: 'target', id: null };
    if (subject.startsWith('kb_core')) return { kind: 'version', id: 'kb-version' };
    return null;
  }

  // The selected drivers a blocker is about: named by it, or on the bus it names.
  function blockerDrivers(findings, image) {
    const out = new Set();
    for (const f of blockers(findings)) {
      const c = causeOf(f.subject, image);
      if (!c) continue;
      if (c.kind === 'driver') out.add(c.id);
      else if (c.kind === 'bus' && c.id) {
        for (const d of image.drivers) if (d.bus === c.id) out.add(d.name);
        const m = /^[^/]+\/(.+)$/.exec(f.subject);
        if (m) out.add(m[1]);
      }
    }
    return out;
  }

  // The panel as numbers and words: the live-region summary and the model the renderer draws.
  function model(ctx, now, lockComponents) {
    const { catalog, image, result, output } = ctx;
    const comps = components(output);
    const ev = (g.KB_EVIDENCE || {})[image.kb_core] || null;
    const sc = cve(g.KB_VULNERABILITIES || null, comps, now);
    const bom = sbom(output, lockComponents);
    const bl = blockers(result.findings);
    const cveText = sc.missing ? 'vulnerability data missing' : sc.stale ? `vulnerability data ${sc.fresh.age} days old`
      : sc.status === 'none known' ? `no known vulnerabilities in ${sc.scanned} of ${comps.length} scanned`
        : sc.status === 'advisories found' || sc.status === 'reviewed'
          ? `${plural(sc.open, 'open vulnerability', 'open vulnerabilities')}${sc.reviewed ? `, ${sc.reviewed} reviewed` : ''}` : 'not scanned';
    return { comps, ev, cve: sc, bom, blockers: bl, details: details(result.findings), safety: safety(ev, image),
      provenance: provenance(ev, catalog.kb_core.repo),
      summary: `${bom.ready ? `${plural(bom.count, 'component')}, ${bom.licences.text}` : 'No SBOM yet'}; ${cveText}; ${bl.length ? plural(bl.length, 'blocker') : 'no blockers'}.` };
  }

  // -- rendering ----------------------------------------------------------------------------------------------
  const api = { now: () => new Date(), model: { freshness, components, cve, licences, sbom, provenance, chips, safety, causeOf, blockerDrivers, blockers, details, summaryOf: model, STALE_DAYS } };
  let lastSummary = '';

  function render(ctx) {
    const doc = g.document;
    const box = doc.getElementById('supply');
    if (!box) return;
    const { h } = ctx;
    const m = model(ctx, api.now(), KB.generate.lockComponents);
    const openOf = (id) => { const e = doc.getElementById(id); return !!(e && e.open); };
    const chip = (tone, text, attrs) => h(attrs && attrs.href ? 'a' : 'span', { class: 'badge ' + tone, ...attrs, text });
    const sec = (id, title, ...kids) => h('section', { class: 'sc', 'aria-labelledby': 'h-' + id }, h('h3', { id: 'h-' + id, text: title }), ...kids);
    const line = (cls, ...kids) => h('p', { class: cls }, ...kids);
    const link = (href, text) => (safeUrl(href) ? h('a', { href, target: '_blank', rel: 'noopener noreferrer', text }) : text);

    // Blockers first, each with a button that moves focus to its cause.
    const blockerBox = m.blockers.length ? h('section', { class: 'sc blockers', 'aria-labelledby': 'h-blockers' },
      h('h3', { id: 'h-blockers', text: `${plural(m.blockers.length, 'blocker')} to fix` }),
      h('ul', { class: 'findings' }, m.blockers.map((f, i) => {
        const cause = causeOf(f.subject, ctx.image);
        return h('li', { class: 'finding error' }, h('span', { class: 'icon', 'aria-hidden': 'true', text: '!' }),
          h('span', {}, h('span', { class: 'visually-hidden', text: 'error: ' }), h('code', { text: f.code }), f.message,
            cause ? [' ', h('button', { type: 'button', class: 'ghost go', id: 'cause-' + i, text: 'Go to cause', onclick: () => focusCause(doc, cause) })] : null));
      }))) : null;

    // SBOM.
    const b = m.bom;
    const sbomBox = sec('sbom', 'SBOM',
      b.ready
        ? h('dl', { class: 'facts' }, h('dt', { text: 'Components' }), h('dd', { text: String(b.count) }), h('dt', { text: 'Licences' }), h('dd', { text: b.licences.text }),
          h('dt', { text: 'Pinned' }), h('dd', { text: `${b.pinned} of ${plural(b.pins, 'version')} by tag and commit${b.dev ? `, ${b.dev} development` : ''}` }))
        : line('note', 'The SBOM is written once the blockers are fixed.'),
      h('div', { class: 'btns' }, ...[['spdx', 'SPDX 2.3'], ['cdx', 'CycloneDX 1.6']].map(([id, label]) => {
        const f = ctx.output.files.find((x) => x.id === id);
        return h('button', { type: 'button', id: 'sbom-' + id, disabled: !b.ready, text: `Download ${label}`,
          onclick: () => { ctx.download(f.path.split('/').pop(), f.text, 'application/json'); ctx.announce(`${label} downloaded.`); } });
      })));

    // CVE.
    const c = m.cve;
    const tone = c.missing || c.status === 'not scanned' ? 'muted' : c.status === 'advisories found' ? 'err' : c.stale ? 'warn' : 'ok';
    const word = c.missing ? 'No data' : c.status === 'advisories found' || c.status === 'reviewed' ? `${c.open} open, ${c.reviewed} reviewed`
      : c.status === 'none known' ? (c.notScanned ? `None known in ${c.scanned} of ${c.items.length}` : 'None known') : 'Not scanned';
    const withOpen = c.items.filter((i) => i.open);
    const withRev = c.items.filter((i) => i.reviewed);
    const unscanned = c.items.filter((i) => i.state === 'not scanned');
    const advLine = (a) => [link(a.url, a.id), a.severity ? ` (${a.severity})` : ''];
    const reviewedBox = (i) => h('details', { id: 'cve-rev-' + i.name, class: 'dis', open: openOf('cve-rev-' + i.name) || null },
      h('summary', { text: `${i.label} ${i.version}: ${i.reviewed} reviewed, fixed in the pinned version` }),
      h('ul', { class: 'adv-list' }, i.advisories.filter((a) => a.review).map((a) => h('li', {}, advLine(a), ' ',
        h('span', { class: 'badge ok', title: a.review.reason, text: reviewWord(a.review) }), ' ', h('span', { class: 'sub', text: a.review.reason }), ' ',
        link(a.review.source, 'source'), a.review.reviewed ? h('span', { class: 'sub', text: ` reviewed ${a.review.reviewed}` }) : null))));
    const cveBox = sec('cve', 'Known vulnerabilities',
      line('chips', chip(tone, word), c.missing ? null : h('span', { class: 'sub', text: `checked ${c.fresh.checked}` })),
      c.missing ? line('warn-line', 'The vulnerability data is missing, so nothing can be said about known vulnerabilities.') : null,
      c.stale ? line('warn-line', `The scan is ${c.fresh.age} days old (more than ${STALE_DAYS}); newer advisories may be missing.`) : null,
      h('ul', { class: 'adv-list' }, withOpen.map((i) => h('li', {}, h('strong', { text: i.label }), ` ${i.version}: `,
        i.advisories.some((a) => !a.review) ? i.advisories.filter((a) => !a.review).map((a, k) => [k ? ', ' : '', advLine(a)])
          : `${i.open} advisory, see the scan`))),
      withRev.map(reviewedBox),
      unscanned.length ? line('note', `Not scanned: ${unscanned.map((i) => i.label).join(', ')}.`) : null,
      h('details', { id: 'cve-all', class: 'dis', open: openOf('cve-all') || null }, h('summary', { text: 'Per component' }),
        h('ul', { class: 'adv-list' }, c.items.map((i) => h('li', {}, h('strong', { text: i.label }), ` ${i.version || ''} `,
          h('span', { class: 'badge ' + (i.state === 'advisories' ? 'err' : i.state === 'not scanned' ? 'muted' : 'ok'),
            text: i.state === 'advisories' ? `${i.open} open${i.reviewed ? `, ${i.reviewed} reviewed` : ''}` : i.state === 'reviewed' ? `0 open, ${i.reviewed} reviewed` : i.state }))))));

    // Provenance.
    const p = m.provenance;
    const provBox = sec('prov', 'Provenance',
      p.state === 'none'
        ? line('note', 'This kb-core revision has no release asset, so there is nothing signed to check.')
        : [line('chips', chip('muted', 'Per release process'), h('span', { class: 'sub', text: `kb-core ${p.version}` })),
          line('note', 'The release workflow attests its files. This page does not check it; verify the download:'),
          h('div', { class: 'cmd' }, h('code', { id: 'verify-cmd', text: p.command }),
            h('button', { type: 'button', id: 'copy-verify', text: 'Copy', 'aria-label': 'Copy the verify command', onclick: () => ctx.copy(p.command, 'Verify command') })),
          p.sha256 ? line('sub', `${p.asset}, sha256 ${p.sha256.slice(0, 12)}...`) : null]);

    // Safety evidence.
    const safe = m.safety;
    const safeBox = sec('safe', 'Safety evidence',
      !safe.length ? line('note', 'No drivers selected.')
        : !m.ev ? line('note', 'No evidence is published for this kb-core revision.')
          : h('ul', { class: 'evi' }, safe.map((d) => h('li', {}, h('strong', { text: d.name }),
            h('span', { class: 'chips' }, d.chips.map((k) => chip(k.tone, k.text, k.href ? { title: k.title, href: k.href, target: '_blank', rel: 'noopener noreferrer' } : { title: k.title }))),
            h('span', { class: 'sub', text: d.label })))));

    // Details: warnings and notes, closed.
    const det = m.details;
    const nW = det.filter((f) => f.severity === 'warning').length;
    const icon = { warning: '!', info: 'i' };
    const detBox = h('details', { id: 'supply-details', class: 'dis', open: openOf('supply-details') || null },
      h('summary', { text: `Details: ${plural(nW, 'warning')}, ${plural(det.length - nW, 'note')}` }),
      h('ul', { class: 'findings' }, det.map((f) => h('li', { class: 'finding ' + f.severity },
        h('span', { class: 'icon', 'aria-hidden': 'true', text: icon[f.severity] }),
        h('span', {}, h('span', { class: 'visually-hidden', text: f.severity + ': ' }), h('code', { text: f.code }), f.mirrors ? h('code', { text: f.mirrors }) : null, f.message)))));

    box.replaceChildren(...[blockerBox, sbomBox, cveBox, provBox, safeBox, detBox,
      h('button', { type: 'button', class: 'primary wide', id: 'panel-zip', disabled: !ctx.output.ready, text: 'Download starter project (.zip)', onclick: ctx.zip })].filter(Boolean));

    // The cause, inline: the chip of each driver a blocker names gets a mark.
    const bad = blockerDrivers(ctx.result.findings, ctx.image);
    for (const d of ctx.image.drivers) {
      const rm = doc.getElementById('tray-rm-' + d.name);
      const li = rm && rm.parentElement;
      if (!li || !bad.has(d.name) || li.querySelector('.blocker')) continue;
      li.classList.add('bad');
      li.insertBefore(h('span', { class: 'badge err blocker', text: 'blocker', title: 'See the panel on the right or at the end of the page' }), rm);
    }

    const sum = doc.getElementById('supply-sum');
    if (sum && m.summary !== lastSummary) { sum.textContent = m.summary; lastSummary = m.summary; }
  }

  function focusCause(doc, cause) {
    const adv = doc.getElementById('advanced');
    const get = (id) => doc.getElementById(id);
    let el = null;
    if (cause.kind === 'driver') el = get('tray-rm-' + cause.id) || get('add-' + cause.id);
    else if (cause.kind === 'rtos') el = doc.querySelector('#rtos input:checked') || doc.querySelector('#rtos input');
    else if (cause.kind === 'target') el = get('target');
    else if (cause.kind === 'version' || cause.kind === 'bus') {
      if (adv) adv.open = true;
      el = cause.kind === 'version' ? get(cause.id) : (cause.id && get('bus-h-' + cause.id)) || get('step-buses');
      if (el && !el.matches('select, button, input, a')) el.setAttribute('tabindex', '-1');
    }
    if (el) { el.scrollIntoView({ block: 'center' }); el.focus(); }
  }

  api.render = render;
  KB.panel = api;
})(globalThis);
