// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The bridge to the shared resolver (resolver.mjs, ADR-0014): derives kb-core's catalogue
// (catalog/schemas/catalog.schema.json) from the site catalogue and kb-core's image (catalog/schemas/kb-image.schema.json) from the
// page's selection, runs the shared resolve(), and merges its findings with the site's own checks so each problem
// is reported once. The shared engine is KB.engine, set by js/boot.mjs (or inlined in offline.html).
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});

  function holders(copyright) {
    const m = /^Copyright \(c\) (?:\d{4}(?:-\d{4})? )?(.+)$/.exec(copyright || '');
    return m ? [m[1]] : [];
  }

  // A version's known-vulnerability scan, or null when it was not scanned (the upgrade planner reports both).
  function osvOf(v) {
    return v && v.osv ? v.osv : null;
  }

  // The version's language floor when the catalogue records one (tools/kb/catalog.py from_site does the same).
  function langOf(v) {
    return v && v.language && typeof v.language === 'object' ? { language: v.language } : {};
  }

  // kb-core's catalogue: the subset of the site catalogue the shared rules (resolver and upgrade planner) read. The
  // kb tool derives the same document from the same file (tools/kb/catalog.py from_site), field for field, so the
  // site and `kb upgrade --catalog data/catalog.json` plan the same thing.
  function kbCatalog(site) {
    return {
      schema: 1,
      kb_core: {
        repo: site.kb_core.repo,
        license: 'MIT',
        authors: ['The kb-core authors'],
        versions: site.kb_core.releases.map((r) => ({ version: r.version, tag: r.tag, commit: r.commit, kind: r.kind, osv: osvOf(r), ...langOf(r) })),
      },
      parts: site.kb_core.parts.slice(),
      rtos: site.rtos.map((r) => ({ name: r.id, status: r.status })),
      mcus: site.families.map((f) => ({
        family: f.id,
        status: f.status,
        targets: f.targets.map((t) => t.id),
        rtos: f.rtos.slice(),
        default_rtos: f.default_rtos,
        port: f.port ? {
          name: f.port.name, repo: f.port.repo, path: f.port.path,
          versions: f.port.releases.map((r) => ({ version: r.version, tag: r.tag, commit: r.commit, kind: r.kind, osv: osvOf(r), ...langOf(r) })),
        } : null,
      })),
      drivers: site.drivers.map((d) => ({
        name: d.name,
        repo: d.repo,
        folder: d.folder,
        pinned: d.pinned,
        versions: d.versions.map((v) => ({
          version: v.version, tag: v.tag, commit: v.commit, kind: v.kind, bus: v.bus.slice(), mcus: v.mcus.slice(),
          peripheral: v.peripheral, kb_core_min: v.kb_core_min,
          requires_owner: typeof v.requires_owner === 'boolean' ? v.requires_owner : d.requires_owner,
          license: d.licence.spdx, authors: holders(d.licence.copyright), osv: osvOf(v), ...langOf(v),
        })),
      })),
    };
  }

  // The bus kind the shared resolver derives from a bus name (resolver.mjs BUS_KIND_RE).
  const NAME_KIND = /^(i2c|spi|uart|can|adc|pio|gpio)[0-9]*$/;

  // kb-core's image (kb-image.yml) of the page's selection.
  function kbImage(image) {
    const out = { schema: 1, image: KB.core.safeName(image.image) };
    if (image.kb_core) out.kb_core = { version: image.kb_core };
    else out.kb_core = {};
    out.mcu = { family: image.mcu.family };
    if (image.mcu.target) out.mcu.target = image.mcu.target;
    if (image.port) out.mcu.port = { version: image.port };
    if (image.rtos) out.rtos = image.rtos;
    if (image.cxx) out.language = { standard: image.cxx };
    out.drivers = (image.drivers || []).map((d) => ({ name: d.name, version: d.version }));
    const buses = {};
    for (const d of image.drivers || []) {
      if (!d.bus) continue;
      const b = (buses[d.bus] = buses[d.bus] || { devices: [], vias: [] });
      b.devices.push(d.name);
      b.vias.push(d.via);
    }
    out.buses = {};
    for (const id of Object.keys(buses).sort()) {
      const model = image.buses && image.buses[id] ? image.buses[id].model : null;
      const bus = { model: model || '', devices: buses[id].devices.slice().sort() };
      const m = NAME_KIND.exec(id);
      const via = buses[id].vias[0];
      if (via && (!m || m[1] !== via)) bus.kind = via; // modbus on uart1, canopen on can0
      out.buses[id] = bus;
    }
    return out;
  }

  // Run both: the shared resolver (when loaded) and the site checks; one list of findings.
  function run(site, image) {
    const res = KB.resolver.resolve(site, image);
    const engine = KB.engine || null;
    let shared = null;
    if (engine) {
      const kbCat = kbCatalog(site);
      const kbImg = kbImage(image);
      const r = engine.resolve(kbCat, kbImg);
      shared = { ok: r.ok, errors: r.errors, warnings: r.warnings, lock: r.lock, files: r.ok ? engine.files(r.lock) : null, image: kbImg, catalog: kbCat };
      // One finding per problem: a shared finding that a site check already words is shown through it.
      const mirrored = new Map();
      for (const f of res.findings) if (f.mirrors) mirrored.set(f.mirrors, (mirrored.get(f.mirrors) || 0) + 1);
      const extra = [];
      for (const [list, severity] of [[r.errors, 'error'], [r.warnings, 'warning']]) {
        const byCode = new Map();
        for (const e of list) byCode.set(e.code, (byCode.get(e.code) || 0) + 1);
        for (const e of list) {
          if ((mirrored.get(e.code) || 0) >= byCode.get(e.code)) continue;
          extra.push({ severity, code: e.code, message: `${e.message} (${e.subject})`, subject: e.subject, mirrors: null, shared: true });
        }
      }
      if (extra.length) {
        const rank = { error: 0, warning: 1, info: 2 };
        res.findings = res.findings.concat(extra).sort((a, b) => rank[a.severity] - rank[b.severity] || (a.code < b.code ? -1 : a.code > b.code ? 1 : 0));
        res.ok = res.ok && !extra.some((f) => f.severity === 'error');
      }
      res.ok = res.ok && r.ok;
    }
    res.shared = shared;
    return res;
  }

  KB.shared = { kbCatalog, kbImage, run, holders };
})(typeof globalThis !== 'undefined' ? globalThis : this);
