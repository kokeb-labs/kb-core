// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The downloads. The shared resolver (resolver.mjs) writes kb-image.lock.yml, CMakePresets.json, kb_image.cmake
// and kb-fetch.sh, byte for byte what `kb resolve` writes; kb-fetch.sh adds every pin to the user's repository as
// a submodule under external/, the one layout of ADR-0014. The site adds kb-image.yml, the SBOMs (CycloneDX 1.6
// and SPDX 2.3, built from the lock the way `kb sbom` builds them, with the SDK and RTOS as SOUP), the licence and status reports and the starter project archive. Every file is
// a pure function of (catalogue, selection): sorted, no clock; the timestamp is the newest commit date of the
// selected components, so the same selection always gives the same bytes.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const { sha256, uuid5, stableJson, yamlScalar, yamlFlow } = KB.core;

  const SITE_VERSION = '0.1.0';
  const EPOCH = '1970-01-01T00:00:00Z';

  // -- the components of a lock (the order and identifiers of tools/kb/sbom.py) ----------------------------------
  function lockComponents(lock) {
    const kb = lock.kb_core;
    const out = [{ ref: 'kb-core', name: 'kb-core', version: kb.version, repo: kb.repo, tag: kb.tag, commit: kb.commit,
      license: kb.license, authors: kb.authors, kind: 'framework', parent: null }];
    for (const part of lock.parts) {
      out.push({ ref: `kb-core/${part}`, name: `kb-${part}`, version: kb.version, repo: kb.repo, tag: kb.tag, commit: kb.commit,
        license: kb.license, authors: kb.authors, kind: 'library', parent: 'kb-core' });
    }
    for (const port of lock.ports) {
      if (port.in_tree) {
        out.push({ ref: `kb-core/port-${port.name}`, name: `port-${port.name}`, version: kb.version, repo: kb.repo, tag: kb.tag,
          commit: kb.commit, license: kb.license, authors: kb.authors, kind: 'library', parent: 'kb-core' });
      } else {
        out.push({ ref: `port-${port.name}`, name: `port-${port.name}`, version: port.version, repo: port.repo, tag: port.tag,
          commit: port.commit, license: 'MIT', authors: [`The port-${port.name} authors`], kind: 'library', parent: null });
      }
    }
    for (const d of lock.drivers) {
      out.push({ ref: `driver-${d.name}`, name: d.folder, version: d.version, repo: d.repo, tag: d.tag, commit: d.commit,
        license: d.license, authors: d.authors, kind: 'library', parent: null });
    }
    return out;
  }

  function soup(lock) {
    const list = [];
    if (lock.mcu.family === 'esp32') list.push({ name: 'esp-idf', version: 'v5.5', licence: 'Apache-2.0', supplier: 'Espressif Systems', note: 'SDK of the ESP32 port' });
    if (lock.rtos === 'freertos') list.push({ name: 'freertos-kernel', version: null, licence: 'MIT', supplier: 'Amazon Web Services', note: lock.mcu.family === 'esp32' ? 'RTOS, via esp-idf' : 'RTOS backend' });
    if (lock.rtos === 'threadx') list.push({ name: 'threadx', version: null, licence: 'MIT', supplier: 'Eclipse Foundation', note: 'RTOS backend; kb-core tests against v6.5.2.202603b_rel (third_party/threadx)' });
    return list;
  }

  function stamp(catalog, res) {
    const ctx = res.context;
    const dates = [];
    if (ctx.kbRelease) dates.push(ctx.kbRelease.date);
    if (ctx.portRelease) dates.push(ctx.portRelease.date);
    for (const c of ctx.chosen) dates.push(c.v.date);
    const sorted = dates.filter(Boolean).sort();
    return sorted.length ? sorted[sorted.length - 1] : (catalog.source_date || EPOCH);
  }

  function purl(repo, tag, commit) {
    return `pkg:github/${repo}@${tag !== null && tag !== undefined ? tag : commit}`;
  }

  function lockDigest(lock) {
    return sha256(KB.engine ? KB.engine.canonicalJson(lock) : stableJson(lock));
  }

  // -- kb-image.yml (catalog/schemas/kb-image.schema.json) ------------------------------------------------------------
  function imageYaml(kbImg) {
    const lines = [
      '# kb-image.yml: the selection of one firmware build (schema 1, docs/design/assembler.md). Written by the',
      '# kb configurator; resolve it with: kb resolve kb-image.yml',
      'schema: 1',
      `image: ${yamlScalar(kbImg.image)}`,
      `kb_core: ${kbImg.kb_core.version ? yamlFlow({ version: kbImg.kb_core.version }) : '{}'}`,
    ];
    const mcu = { family: kbImg.mcu.family };
    if (kbImg.mcu.target) mcu.target = kbImg.mcu.target;
    let mcuText = yamlFlow(mcu);
    if (kbImg.mcu.port) mcuText = mcuText.replace(/\}$/, `, port: ${yamlFlow(kbImg.mcu.port)}}`);
    lines.push(`mcu: ${mcuText}`);
    if (kbImg.rtos) lines.push(`rtos: ${yamlScalar(kbImg.rtos)}`);
    if (kbImg.language) lines.push(`language: ${yamlFlow(kbImg.language)}`);
    if (!kbImg.drivers.length) lines.push('drivers: []');
    else {
      lines.push('drivers:');
      for (const d of kbImg.drivers.slice().sort((a, b) => (a.name < b.name ? -1 : 1))) lines.push(`  - ${yamlFlow(d)}`);
    }
    const ids = Object.keys(kbImg.buses);
    if (!ids.length) lines.push('buses: {}');
    else {
      lines.push('buses:');
      for (const id of ids) {
        const b = kbImg.buses[id];
        const o = { model: b.model || 'none' };
        if (b.kind) o.kind = b.kind;
        o.devices = b.devices;
        lines.push(`  ${id}: ${yamlFlow(o)}`);
      }
    }
    return lines.join('\n') + '\n';
  }

  // -- SBOMs --------------------------------------------------------------------------------------------------
  function cdxComponent(c, osv) {
    const comp = {
      type: c.kind,
      'bom-ref': c.ref,
      name: c.name,
      version: c.version,
      purl: purl(c.repo, c.tag, c.commit),
      externalReferences: [{ type: 'vcs', url: `https://github.com/${c.repo}` }],
      properties: [{ name: 'kb:commit', value: c.commit }],
    };
    if (osv !== undefined) comp.properties.push({ name: 'kb:osv', value: osv ? `scanned ${osv.scanned_at} (${osv.database}), ${osv.findings.length} finding(s)` : 'not scanned' });
    if (c.license !== 'NOASSERTION') comp.licenses = [{ license: { id: c.license } }];
    if (c.authors.length) comp.authors = c.authors.map((a) => ({ name: a }));
    return comp;
  }

  function osvOf(res, c) {
    const ctx = res.context;
    if (c.ref === 'kb-core') return ctx.kbRelease ? ctx.kbRelease.osv : null;
    if (c.ref.startsWith('port-')) return ctx.portRelease ? ctx.portRelease.osv : null;
    if (c.ref.startsWith('driver-')) {
      const ch = ctx.chosen.find((x) => `driver-${x.d.name}` === c.ref);
      return ch ? ch.v.osv : null;
    }
    return undefined; // parts and in-tree ports share kb-core's scan
  }

  function cyclonedx(lock, res, created) {
    const comps = lockComponents(lock);
    const top = comps.filter((c) => c.parent === null).map((c) => cdxComponent(c, osvOf(res, c)));
    for (const c of comps.filter((x) => x.parent !== null)) {
      const parent = top.find((t) => t['bom-ref'] === c.parent);
      (parent.components = parent.components || []).push(cdxComponent(c, undefined));
    }
    for (const s of soup(lock)) {
      top.push({
        type: 'framework', 'bom-ref': `soup:${s.name}`, name: s.name, version: s.version || 'unspecified',
        supplier: { name: s.supplier }, licenses: [{ license: { id: s.licence } }],
        properties: [{ name: 'kb:role', value: 'soup' }, { name: 'kb:note', value: s.note }],
      });
    }
    const imageRef = `image:${lock.image}`;
    return stableJson({
      bomFormat: 'CycloneDX',
      specVersion: '1.6',
      serialNumber: `urn:uuid:${uuid5('kb-lock:' + lockDigest(lock))}`,
      version: 1,
      metadata: {
        timestamp: created,
        component: { type: 'firmware', 'bom-ref': imageRef, name: lock.image,
          properties: [{ name: 'kb:mcu', value: `${lock.mcu.family}/${lock.mcu.target}` }, { name: 'kb:rtos', value: lock.rtos }] },
        tools: { components: [{ type: 'application', name: 'kb-configurator', version: SITE_VERSION }] },
      },
      components: top,
      dependencies: [{ ref: imageRef, dependsOn: top.map((t) => t['bom-ref']).sort() }],
    });
  }

  function spdxId(ref) {
    return 'SPDXRef-' + ref.replace(/[^A-Za-z0-9.-]/g, '-');
  }

  function spdx(lock, res, created) {
    const digest = lockDigest(lock);
    const imageId = spdxId(`image-${lock.image}`);
    const packages = [{ SPDXID: imageId, name: lock.image, downloadLocation: 'NOASSERTION', filesAnalyzed: false,
      licenseConcluded: 'NOASSERTION', licenseDeclared: 'NOASSERTION', copyrightText: 'NOASSERTION', primaryPackagePurpose: 'FIRMWARE' }];
    const relationships = [{ spdxElementId: 'SPDXRef-DOCUMENT', relationshipType: 'DESCRIBES', relatedSpdxElement: imageId }];
    for (const c of lockComponents(lock)) {
      const ref = c.tag !== null && c.tag !== undefined ? c.tag : c.commit;
      const holders = c.authors.join(', ');
      const osv = osvOf(res, c);
      packages.push({
        SPDXID: spdxId(c.ref), name: c.name, versionInfo: c.version,
        downloadLocation: `git+https://github.com/${c.repo}.git@${ref}`, filesAnalyzed: false,
        licenseConcluded: c.license, licenseDeclared: c.license,
        copyrightText: holders ? `Copyright (c) ${holders}` : 'NOASSERTION',
        supplier: 'Organization: ' + c.repo.split('/')[0],
        primaryPackagePurpose: c.kind === 'library' ? 'LIBRARY' : 'FRAMEWORK',
        externalRefs: [{ referenceCategory: 'PACKAGE-MANAGER', referenceType: 'purl', referenceLocator: purl(c.repo, c.tag, c.commit) }],
        comment: `commit ${c.commit}` + (osv === undefined ? '' : `; known-vulnerability scan: ${osv ? osv.scanned_at : 'not scanned'}`),
      });
      relationships.push({ spdxElementId: c.parent === null ? imageId : spdxId(c.parent), relationshipType: 'CONTAINS', relatedSpdxElement: spdxId(c.ref) });
    }
    for (const s of soup(lock)) {
      const id = spdxId(`soup-${s.name}`);
      packages.push({ SPDXID: id, name: s.name, versionInfo: s.version || 'NOASSERTION', downloadLocation: 'NOASSERTION',
        filesAnalyzed: false, licenseConcluded: s.licence, licenseDeclared: s.licence, copyrightText: 'NOASSERTION',
        supplier: `Organization: ${s.supplier}`, primaryPackagePurpose: 'FRAMEWORK', comment: `SOUP, ${s.note}` });
      relationships.push({ spdxElementId: imageId, relationshipType: 'CONTAINS', relatedSpdxElement: id });
    }
    return stableJson({
      spdxVersion: 'SPDX-2.3',
      dataLicense: 'CC0-1.0',
      SPDXID: 'SPDXRef-DOCUMENT',
      name: `kb-image-${lock.image}`,
      documentNamespace: `https://kokeb-labs.github.io/kb-core/spdx/${lock.image}-${digest}`,
      creationInfo: { created, creators: [`Tool: kb-configurator-${SITE_VERSION}`] },
      packages,
      relationships,
    });
  }

  // -- reports ----------------------------------------------------------------------------------------------
  function mdCell(s) {
    return String(s === null || s === undefined ? '' : s).replace(/\|/g, '\\|');
  }

  // One row per selected component, from the site catalogue (works before the lock exists).
  function rows(res) {
    const ctx = res.context;
    const out = [];
    if (ctx.kbRelease) out.push({ name: 'kb-core', version: ctx.kbRelease.version, kind: ctx.kbRelease.kind, licence: 'MIT', copyright: 'Copyright (c) 2026 The kb-core authors', osv: ctx.kbRelease.osv });
    if (ctx.portRelease) out.push({ name: `port-${ctx.family.port.name}`, version: ctx.portRelease.version, kind: ctx.portRelease.kind, licence: 'MIT', copyright: `Copyright (c) 2026 The port-${ctx.family.port.name} authors`, osv: ctx.portRelease.osv });
    for (const c of ctx.chosen.slice().sort((a, b) => (a.d.name < b.d.name ? -1 : 1))) {
      out.push({ name: c.d.folder, version: c.v.version, kind: c.v.kind, licence: c.d.licence.spdx, copyright: c.d.licence.copyright, osv: c.v.osv, c });
    }
    return out;
  }

  function soupFor(res) {
    const ctx = res.context;
    return soup({ mcu: { family: ctx.family ? ctx.family.id : null }, rtos: ctx.rtos ? ctx.rtos.id : null });
  }

  function licenceReport(name, res) {
    const lines = [`# Licences of the image ${name}`, '', 'Each component\'s SPDX licence and copyright holder, from its LICENSE file at the pinned tag.', '',
      '| Component | Version | Licence (SPDX) | Copyright |', '| --- | --- | --- | --- |'];
    for (const r of rows(res)) lines.push(`| ${mdCell(r.name)} | ${mdCell(r.version)} | ${mdCell(r.licence)} | ${mdCell(r.copyright)} |`);
    for (const s of soupFor(res)) lines.push(`| ${mdCell(s.name)} (SOUP, ${mdCell(s.note)}) | ${mdCell(s.version || 'from the SDK')} | ${mdCell(s.licence)} | ${mdCell(s.supplier)} |`);
    const set = [...new Set(rows(res).map((r) => r.licence).concat(soupFor(res).map((s) => s.licence)))].sort();
    lines.push('', `Licences in this image: ${set.join(', ') || 'none'}.`);
    return lines.join('\n') + '\n';
  }

  function statusReport(name, res) {
    const lines = [`# Status of the image ${name}`, '', '## Tested version set', ''];
    if (res.resolved.tested_sets.length) lines.push(`Tested: this combination is in ${res.resolved.tested_sets.join(', ')}.`);
    else lines.push('**Untested combination.** No tested version set holds exactly these versions on this MCU, target and RTOS.');
    lines.push('', '## Known vulnerabilities', '', '| Component | Version | Scan |', '| --- | --- | --- |');
    for (const r of rows(res)) {
      const scan = r.osv ? `${r.osv.scanned_at}, ${r.osv.database}: ${r.osv.findings.length ? r.osv.findings.map((f) => `${f.id} (${f.severity})`).join(', ') : 'no known vulnerabilities'}` : 'not scanned';
      lines.push(`| ${mdCell(r.name)} | ${mdCell(r.version)} | ${mdCell(scan)} |`);
    }
    lines.push('', '## Compatibility rows', '', '| Driver | Version | Kind | Bus used | Buses | MCUs | Peripheral | kb-core min | Needs owner task |', '| --- | --- | --- | --- | --- | --- | --- | --- | --- |');
    for (const r of rows(res).filter((x) => x.c)) {
      const { c } = r;
      lines.push(`| ${c.d.name} | ${c.v.version} | ${c.v.kind} | ${c.sel.via} on ${c.sel.bus} | ${c.v.bus.join(', ')} | ${c.v.mcus.join(', ')} | ${c.v.peripheral || '-'} | ${c.v.kb_core_min || '-'} | ${c.d.requires_owner ? 'yes' : 'no'} |`);
    }
    lines.push('', '## Findings', '');
    if (!res.findings.length) lines.push('None.');
    for (const f of res.findings) lines.push(`- ${f.severity} ${f.code}${f.mirrors ? ` (${f.mirrors})` : ''}: ${f.message}`);
    return lines.join('\n') + '\n';
  }

  // -- starter project ----------------------------------------------------------------------------------------
  function projectFiles(lock, res) {
    const name = lock.image;
    const fam = lock.mcu.family;
    const files = [];
    const buses = res.resolved.buses;
    const busLines = Object.keys(buses).map((id) => `//   ${id}: ${buses[id].model} (${buses[id].devices.join(', ')})`);
    if (fam === 'esp32') {
      const port = lock.ports[0];
      const portDir = port.in_tree ? `external/kb-core/${port.path}` : `external/port-${port.name}`;
      files.push({ path: 'CMakeLists.txt', text: [
        `# ESP-IDF project of the image ${name}. kb-core is the ESP-IDF component "kb" (ADR-0008); kb-fetch.sh puts`,
        '# it and the drivers under external/, and kb_image.cmake points KB_DRIVER_<NAME>_DIR at them.',
        'cmake_minimum_required(VERSION 3.16)',
        'include(${CMAKE_CURRENT_LIST_DIR}/kb_image.cmake)',
        `set(EXTRA_COMPONENT_DIRS \${CMAKE_CURRENT_LIST_DIR}/external/kb-core \${CMAKE_CURRENT_LIST_DIR}/${portDir})`,
        'include($ENV{IDF_PATH}/tools/cmake/project.cmake)',
        `project(${name.replace(/-/g, '_')})`,
      ].join('\n') + '\n' });
      files.push({ path: 'main/CMakeLists.txt', text: 'idf_component_register(SRCS "main.cpp" "board.cpp" REQUIRES kb)\n' });
      files.push({ path: 'main/board.cpp', text: [
        `// Board layer of the image ${name}: declare each bus with its thread model (ADR-0009) and bind the handlers.`,
        ...busLines, 'namespace board {', 'void Start() {}', '}  // namespace board'].join('\n') + '\n' });
      files.push({ path: 'main/main.cpp', text: [`// Entry point of the image ${name}.`, 'namespace board { void Start(); }',
        'extern "C" void app_main(void) { board::Start(); }'].join('\n') + '\n' });
    } else {
      const boardParts = lock.parts.map((p) => `kb::${p}`).join(' ');
      files.push({ path: 'CMakeLists.txt', text: [
        `# Project of the image ${name}.`,
        'cmake_minimum_required(VERSION 3.25)',
        `project(${name.replace(/-/g, '_')} LANGUAGES CXX)`,
        'set(CMAKE_CXX_STANDARD 20)',
        'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
        'include(${CMAKE_CURRENT_LIST_DIR}/kb_image.cmake)  # KB_MCU, KB_RTOS, KB_PARTS, KB_ENABLE_*, KB_DRIVER_*_DIR',
        'add_subdirectory(external/kb-core)',
        '# The board layer owns the buses, the drivers and their handlers; the application sees only the board',
        '# and kb::utils / kb::rtos (applications never use kb::interfaces or kb::handlers, ADR-0001).',
        'add_library(board STATIC board/board.cpp)',
        `target_link_libraries(board PUBLIC ${boardParts})`,
        'add_executable(app src/main.cpp)',
        'target_link_libraries(app PRIVATE board kb::utils kb::rtos)',
      ].join('\n') + '\n' });
      files.push({ path: 'board/board.cpp', text: [
        `// Board layer of the image ${name}: declare each bus with its thread model (ADR-0009) and bind the handlers.`,
        ...busLines, 'namespace board {', 'void Start() {}', '}  // namespace board'].join('\n') + '\n' });
      files.push({ path: 'src/main.cpp', text: [`// Entry point of the image ${name}.`, 'namespace board { void Start(); }',
        'int main() {', '  board::Start();', '  return 0;', '}'].join('\n') + '\n' });
    }
    files.push({ path: '.gitignore', text: 'build/\nsdkconfig\nsdkconfig.old\n' });
    const open = res.findings.filter((f) => f.severity !== 'info');
    files.push({ path: 'README.md', text: [
      `# ${name}`, '',
      `A starter project for ${res.context.target ? res.context.target.label : lock.mcu.target} with ${res.context.rtos ? res.context.rtos.label : lock.rtos}, made by the kb configurator.`, '',
      '1. `git init` (if this is not a repository yet), then `sh kb-fetch.sh`: it adds kb-core, the port and every',
      '   driver as submodules under `external/` at their tags and checks each commit against `kb-image.lock.yml`.',
      `2. Configure: \`cmake --preset ${name}\`${fam === 'esp32' ? ' with ESP-IDF v5.5 in the environment, or `idf.py build`' : ''}.`,
      '3. In CI, `sh kb-fetch.sh --verify` keeps every pin honest. `kb resolve kb-image.yml` writes the same files.', '',
      'Files: `kb-image.yml` (the selection), `kb-image.lock.yml` (the pins), `CMakePresets.json`, `kb_image.cmake`,',
      'and the evidence in `evidence/` (SBOM in CycloneDX 1.6 and SPDX 2.3, licences, status).', '',
      ...(open.length ? ['## Open points', '', ...open.map((f) => `- ${f.code}: ${f.message}`), ''] : []),
    ].join('\n') });
    return files;
  }

  const UNAVAILABLE = (what) => `# ${what} is written once the selection resolves: fix the errors listed under Check.\n`;

  function generate(catalog, image, res) {
    const shared = res.shared;
    const kbImg = shared ? shared.image : KB.shared.kbImage(image);
    const name = kbImg.image;
    const lock = shared && shared.ok ? shared.lock : null;
    const created = stamp(catalog, res);
    const sf = shared && shared.files ? shared.files : null;
    const engineFile = (key, what) => (sf ? sf[key] : UNAVAILABLE(what));
    const files = [
      { id: 'image', label: 'kb-image.yml', path: 'kb-image.yml', lang: 'yaml', text: imageYaml(kbImg) },
      { id: 'lock', label: 'Lock', path: 'kb-image.lock.yml', lang: 'yaml', text: engineFile('kb-image.lock.yml', 'The lock') },
      { id: 'presets', label: 'CMake presets', path: 'CMakePresets.json', lang: 'json', text: engineFile('CMakePresets.json', 'CMakePresets.json') },
      { id: 'cmake', label: 'kb_image.cmake', path: 'kb_image.cmake', lang: 'cmake', text: engineFile('kb_image.cmake', 'kb_image.cmake') },
      { id: 'fetch', label: 'kb-fetch.sh', path: 'kb-fetch.sh', lang: 'sh', text: engineFile('kb-fetch.sh', 'kb-fetch.sh'), mode: 0o755 },
      { id: 'cdx', label: 'CycloneDX', path: 'evidence/sbom.cdx.json', lang: 'json', text: lock ? cyclonedx(lock, res, created) : UNAVAILABLE('The CycloneDX SBOM') },
      { id: 'spdx', label: 'SPDX', path: 'evidence/sbom.spdx.json', lang: 'json', text: lock ? spdx(lock, res, created) : UNAVAILABLE('The SPDX SBOM') },
      { id: 'licences', label: 'Licences', path: 'evidence/licences.md', lang: 'md', text: licenceReport(name, res) },
      { id: 'status', label: 'Status', path: 'evidence/status.md', lang: 'md', text: statusReport(name, res) },
    ];
    return { files, project: lock ? projectFiles(lock, res) : [], stamp: created, name, lock, rows: rows(res), soup: soupFor(res), ready: !!lock && res.ok };
  }

  // The starter project as a ZIP. Deterministic: sorted entries, a fixed time, fixed permissions.
  function zipProject(fflate, out) {
    const root = out.name + '/';
    // ZIP stores local time fields; build a Date whose local fields are the UTC stamp, so every time zone
    // writes the same bytes.
    const u = new Date(out.stamp < '1980-01-02' ? '1980-01-02T00:00:00Z' : out.stamp);
    const when = new Date(u.getUTCFullYear(), u.getUTCMonth(), u.getUTCDate(), u.getUTCHours(), u.getUTCMinutes(), u.getUTCSeconds());
    const entries = {};
    const all = out.files.concat(out.project).slice().sort((a, b) => (a.path < b.path ? -1 : 1));
    for (const f of all) {
      entries[root + f.path] = [KB.core.utf8(f.text), { level: 9, mtime: when, os: 3, attrs: ((f.mode || 0o644) | 0o100000) << 16 }];
    }
    return fflate.zipSync(entries, { level: 9, mtime: when });
  }

  KB.generate = { generate, zipProject, lockComponents, SITE_VERSION };
})(typeof globalThis !== 'undefined' ? globalThis : this);
