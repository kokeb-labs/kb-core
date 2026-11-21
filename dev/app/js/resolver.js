// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The site checks: a selection and the site catalogue in, findings in plain words and the resolved build out. It
// covers the shared rules of docs/design/assembler.md in the site's own terms (each with the KB-RES code it
// mirrors, so the page shows one finding per problem when the shared resolver also runs) and adds what only the
// site knows: bus instances of the target, point-to-point lines, I2C addresses, the pin budget, handler bindings,
// tested sets and known vulnerabilities. Specification: docs/configurator-checks.md. Pure function, no DOM.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const { compareVersions, satisfies } = KB.core;

  const BUS_ID_RE = /^([a-z][a-z0-9]*?)(\d+)$/;

  // Site check -> the shared rule it mirrors (docs/design/assembler.md).
  const MIRRORS = {
    'CFG-MCU-001': 'KB-RES-003', 'CFG-MCU-002': 'KB-RES-005', 'CFG-MCU-004': 'KB-RES-004',
    'CFG-RTOS-001': 'KB-RES-008', 'CFG-RTOS-002': 'KB-RES-007', 'CFG-RTOS-003': 'KB-RES-006',
    'CFG-KB-001': 'KB-RES-001', 'CFG-KB-002': 'KB-RES-002', 'CFG-KB-003': 'KB-RES-W03', 'CFG-PORT-002': 'KB-RES-020', 'CFG-RC-001': 'KB-RES-W01',
    'CFG-DRV-001': 'KB-RES-009', 'CFG-DRV-002': 'KB-RES-010', 'CFG-DRV-003': 'KB-RES-011',
    'CFG-DRV-010': 'KB-RES-013', 'CFG-DRV-011': 'KB-RES-014', 'CFG-BUS-001': 'KB-RES-019',
    'CFG-THR-001': 'KB-RES-015', 'CFG-THR-002': 'KB-RES-018',
  };

  function index(list, key) {
    const m = new Map();
    for (const item of list || []) m.set(item[key || 'id'], item);
    return m;
  }

  function hex(n) {
    return '0x' + n.toString(16).toUpperCase().padStart(2, '0');
  }

  // Can every device on one I2C bus get its own address? Small backtracking search (a bus holds few devices).
  function assignAddresses(devices) {
    const order = devices.slice().sort((a, b) => a.addresses.length - b.addresses.length || (a.name < b.name ? -1 : 1));
    const used = new Set();
    const out = {};
    function step(i) {
      if (i === order.length) return true;
      for (const addr of order[i].addresses) {
        if (used.has(addr)) continue;
        used.add(addr);
        out[order[i].name] = addr;
        if (step(i + 1)) return true;
        used.delete(addr);
      }
      return false;
    }
    return step(0) ? out : null;
  }

  // The default version: kb-core's pin when catalogued, else the newest release, else the newest rc (ADR-0014).
  function latestVersion(driver) {
    if (driver.pinned) {
      const pinned = driver.versions.find((v) => v.version === driver.pinned);
      if (pinned) return pinned;
    }
    const rel = driver.versions.filter((v) => v.kind === 'release');
    const pool = rel.length ? rel : driver.versions;
    return pool.slice().sort((a, b) => compareVersions(b.version, a.version))[0] || null;
  }

  function resolve(catalog, image) {
    const findings = [];
    const add = (severity, code, message, subject) => findings.push({ severity, code, message, subject: subject || null, mirrors: MIRRORS[code] || null });
    const err = (c, m, s) => add('error', c, m, s);
    const warn = (c, m, s) => add('warning', c, m, s);
    const info = (c, m, s) => add('info', c, m, s);

    const families = index(catalog.families);
    const rtosList = index(catalog.rtos);
    const driversById = index(catalog.drivers, 'name');
    const kinds = catalog.bus_kinds;

    // -- platform --------------------------------------------------------------------------------------------
    const mcu = image.mcu || {};
    const family = families.get(mcu.family) || null;
    let target = null;
    let board = null;
    if (!family) {
      err('CFG-MCU-001', `There is no MCU family "${mcu.family}" in the catalogue.`, 'mcu.family');
    } else {
      if (family.status === 'planned' || !family.port) {
        err('CFG-MCU-004', `${family.label} is planned: kb-core has no port for it yet${family.port_planned ? ` (it will be ${family.port_planned})` : ''}, so nothing can be built for it today.`, 'mcu.family');
      }
      target = index(family.targets).get(mcu.target) || null;
      if (!target) err('CFG-MCU-002', `${family.label} has no target "${mcu.target}"; it has ${family.targets.map((t) => t.id).join(', ')}.`, 'mcu.target');
      board = index(family.boards).get(mcu.board) || null;
      if (mcu.board && !board) err('CFG-MCU-003', `There is no board "${mcu.board}" for ${family.label}.`, 'mcu.board');
      if (board && target && board.target !== target.id) {
        const carried = index(family.targets).get(board.target);
        err('CFG-MCU-003', `${board.label} is built around the ${carried ? carried.label : board.target}, not the ${target.label}.`, 'mcu.board');
      }
    }

    const rtos = rtosList.get(image.rtos) || null;
    if (!rtos) {
      err('CFG-RTOS-003', `There is no RTOS backend "${image.rtos}".`, 'rtos');
    } else if (rtos.status === 'planned') {
      err('CFG-RTOS-002', `The ${rtos.label} backend of kb::rtos is planned and not implemented yet.`, 'rtos');
    } else if (family && !family.rtos.includes(rtos.id)) {
      err('CFG-RTOS-001', `${family.label} cannot use the "${rtos.label}" backend. ${family.rtos_note || ''}`.trim(), 'rtos');
    }

    // -- kb-core and the port ----------------------------------------------------------------------------------
    let kbRelease = null;
    if (image.kb_core === null || image.kb_core === undefined) {
      err('CFG-KB-001', 'No kb-core version is selected, and the catalogue has none: there is nothing to build on yet.', 'kb_core.version');
    } else {
      kbRelease = catalog.kb_core.releases.find((r) => r.version === image.kb_core) || null;
      if (!kbRelease) err('CFG-KB-002', `kb-core ${image.kb_core} is not in the catalogue.`, 'kb_core.version');
      else if (kbRelease.kind === 'rc') warn('CFG-RC-001', `kb-core ${kbRelease.version} is a release candidate.`, 'kb-core');
      else if (kbRelease.kind === 'dev') {
        warn('CFG-KB-003', `kb-core has no release yet: ${kbRelease.version} is the development revision ${kbRelease.commit.slice(0, 12)}, pinned by commit instead of a tag.`, 'kb-core');
      }
    }

    let portRelease = null;
    let portInTree = false;
    if (family && family.port) {
      const port = family.port;
      if (!port.releases.length) {
        portInTree = true;
        if (image.port !== null && image.port !== undefined && image.port !== image.kb_core) {
          err('CFG-PORT-002', `The ${port.name} port is inside kb-core (${port.path}) and has kb-core's version, not ${image.port}.`, 'mcu.port.version');
        }
        if (family.id !== 'host') info('CFG-PORT-004', `The ${port.name} port is still inside kb-core (${port.path}); it comes with kb-core until ${port.repo} is published.`, `port-${port.name}`);
      } else {
        const want = image.port === null || image.port === undefined ? null : image.port;
        portRelease = want === null ? latestVersion({ versions: port.releases })
          : port.releases.find((r) => r.version === want) || null;
        if (!portRelease) err('CFG-PORT-002', `port-${port.name} has no version ${want}.`, 'mcu.port.version');
        else {
          if (portRelease.kind === 'rc') warn('CFG-RC-001', `port-${port.name} ${portRelease.version} is a release candidate.`, `port-${port.name}`);
          if (kbRelease && portRelease.kb_core && !satisfies(kbRelease.version, portRelease.kb_core)) {
            err('CFG-PORT-003', `port-${port.name} ${portRelease.version} works with kb-core ${portRelease.kb_core}, not ${kbRelease.version}.`, `port-${port.name}`);
          }
        }
      }
    }

    // -- drivers -----------------------------------------------------------------------------------------------
    const chosen = [];
    const seen = new Set();
    for (const sel of image.drivers || []) {
      const d = driversById.get(sel.name);
      if (seen.has(sel.name)) { err('CFG-DRV-002', `${sel.name} is selected twice; one driver serves every chip of its kind on a bus.`, sel.name); continue; }
      seen.add(sel.name);
      if (!d) { err('CFG-DRV-001', `There is no driver "${sel.name}" in the catalogue.`, sel.name); continue; }
      if (!d.versions.length) { err('CFG-DRV-003', `${d.name} has no published release or release candidate yet, so it cannot be pinned.`, d.name); continue; }
      const v = d.versions.find((x) => x.version === sel.version);
      if (!v) { err('CFG-DRV-003', `${d.name} has no version ${sel.version}. Published: ${d.versions.map((x) => x.version).join(', ')}.`, d.name); continue; }
      if (v.kind === 'rc') warn('CFG-RC-001', `${d.name} ${v.version} is a release candidate, not a final release.`, d.name);
      if (family && !v.mcus.includes('any') && !v.mcus.includes(family.id)) {
        const need = v.mcus.map((m) => (families.get(m) ? families.get(m).label : m)).join(' or ');
        err('CFG-DRV-010', `${d.name} needs ${need}${v.peripheral ? ` (its ${v.peripheral.toUpperCase()} peripheral)` : ''}; it cannot run on ${family.label}.`, d.name);
      }
      if (v.kb_core_min) {
        if (kbRelease && compareVersions(kbRelease.version, v.kb_core_min) < 0) {
          err('CFG-DRV-011', `${d.name} ${v.version} needs kb-core ${v.kb_core_min} or newer; ${kbRelease.version} is selected.`, d.name);
        } else if (!kbRelease) {
          info('CFG-DRV-012', `${d.name} ${v.version} needs kb-core ${v.kb_core_min} or newer; check it once kb-core is chosen.`, d.name);
        }
      }
      if (kbRelease && kbRelease.kind !== 'dev') {
        if (!(kbRelease.bindings || []).includes(d.name)) {
          err('CFG-HND-001', `kb-core ${kbRelease.version} has no handler binding for ${d.name}, so KB_ENABLE_${d.name.toUpperCase()} would not build.`, d.name);
        }
      } else if (kbRelease) {
        info('CFG-HND-002', `The handler binding of ${d.name} cannot be checked against a development revision of kb-core.`, d.name);
      }
      chosen.push({ sel, d, v });
    }

    // -- buses -------------------------------------------------------------------------------------------------
    const buses = {};
    const counts = (target && target.buses) || {};
    for (const c of chosen) {
      const { sel, d, v } = c;
      const via = sel.via;
      if (!via || !v.bus.includes(via) || !kinds[via]) {
        err('CFG-BUS-001', `${d.name} talks over ${v.bus.join(' or ')}; "${via || 'nothing'}" is not one of them.`, d.name);
        continue;
      }
      const kind = kinds[via];
      const m = BUS_ID_RE.exec(sel.bus || '');
      if (!m || m[1] !== kind.instance) {
        err('CFG-BUS-002', `${d.name} uses ${kind.label}, so it must sit on a bus named ${kind.instance}<n> (for example ${kind.instance}0), not "${sel.bus || ''}".`, d.name);
        continue;
      }
      const available = counts[kind.instance];
      if (target && available !== null && available !== undefined && kind.instance !== 'adc' && kind.instance !== 'gpio' && +m[2] >= available) {
        err('CFG-BUS-003', available === 0
          ? `${target.label} has no ${kind.instance.toUpperCase()} peripheral, so ${d.name} cannot be wired here.`
          : `${target.label} has ${available} ${kind.instance.toUpperCase()} instance${available === 1 ? '' : 's'} (${kind.instance}0${available > 1 ? ` to ${kind.instance}${available - 1}` : ''}); ${sel.bus} does not exist.`, d.name);
      }
      const b = (buses[sel.bus] = buses[sel.bus] || { id: sel.bus, kind: kind.instance, vias: new Set(), devices: [] });
      b.vias.add(via);
      b.devices.push(c);
      c.busKind = kind;
    }

    const models = image.buses || {};
    for (const id of Object.keys(models).sort()) {
      if (!buses[id]) warn('CFG-BUS-008', `Bus ${id} has a thread model but no device on it; it is left out.`, `buses.${id}`);
    }
    let adcDevices = 0;
    for (const id of Object.keys(buses).sort()) {
      const b = buses[id];
      const names = b.devices.map((c) => c.d.name).sort();
      if (b.vias.size > 1) {
        err('CFG-BUS-005', `${id} carries ${[...b.vias].sort().join(' and ')} at once; one line runs one protocol. Move one of ${names.join(', ')} to another ${b.kind}.`, `buses.${id}`);
      }
      const shared = [...b.vias].every((via) => kinds[via].shared);
      if (!shared && b.devices.length > 1) {
        err('CFG-BUS-004', `${id} is point to point: it can serve one driver, but ${names.join(' and ')} are on it.`, `buses.${id}`);
      }
      if (b.kind === 'adc') adcDevices += b.devices.length;
      // Thread model: one per bus (ADR-0009).
      const model = models[id] && models[id].model;
      b.model = null;
      if (model !== 'owner' && model !== 'guarded') {
        err('CFG-THR-001', `${id} needs one thread model, owner or guarded, for ${names.join(', ')}.`, `buses.${id}.model`);
      } else {
        b.model = model;
        if (model === 'guarded') {
          for (const c of b.devices) {
            if (c.d.requires_owner) {
              err('CFG-THR-002', `${c.d.name} must be serviced continuously, so its bus needs an owner task; ${id} is guarded. Make ${id} an owner bus or move ${c.d.name}.`, `${id}/${c.d.name}`);
            }
          }
        }
        if (model === 'owner' && rtos && rtos.id === 'none') {
          info('CFG-THR-003', `With no RTOS, the owner of ${id} is your main loop: call Service() on it every step.`, `buses.${id}`);
        }
      }
      if (b.kind === 'i2c' && b.devices.length > 1) {
        const known = b.devices.filter((c) => Array.isArray(c.d.i2c_addresses) && c.d.i2c_addresses.length);
        const unknown = b.devices.filter((c) => !known.includes(c)).map((c) => c.d.name).sort();
        if (known.length > 1) {
          const assignment = assignAddresses(known.map((c) => ({ name: c.d.name, addresses: c.d.i2c_addresses })));
          if (!assignment) {
            err('CFG-BUS-010', `${known.map((c) => c.d.name).sort().join(' and ')} cannot all have their own address on ${id}: their address choices overlap. Put one on another I2C bus.`, `buses.${id}`);
          } else {
            b.addresses = assignment;
            if (known.some((c) => c.d.i2c_addresses.length > 1)) {
              info('CFG-BUS-011', `On ${id}, give the chips different addresses, for example ${Object.keys(assignment).sort().map((n) => `${n} ${hex(assignment[n])}`).join(', ')}.`, `buses.${id}`);
            }
          }
        }
        if (unknown.length) info('CFG-BUS-012', `The I2C address of ${unknown.join(', ')} is not in the catalogue; check it does not clash on ${id}.`, `buses.${id}`);
      }
    }
    if (target && counts.adc !== null && counts.adc !== undefined && adcDevices > counts.adc) {
      err('CFG-BUS-013', `${adcDevices} ADC inputs are needed but ${target.label} has ${counts.adc} ADC channels.`, 'adc');
    }

    // -- pins --------------------------------------------------------------------------------------------------
    let minPins = 0;
    let optional = 0;
    for (const id of Object.keys(buses)) {
      const b = buses[id];
      minPins += Math.max(...b.devices.map((c) => c.busKind.bus_pins));
      minPins += b.devices.length * b.devices[0].busKind.pins_per_device;
      for (const c of b.devices) optional += c.d.optional_pins || 0;
    }
    const budget = board && board.usable_gpio !== null && board.usable_gpio !== undefined ? board.usable_gpio : null;
    const ceiling = target && target.buses && typeof target.buses.gpio === 'number' ? target.buses.gpio : null;
    if (chosen.length) {
      if (budget !== null) {
        if (minPins > budget) {
          err('CFG-GPIO-001', `The buses need at least ${minPins} pins but ${board.label} has about ${budget} free.`, 'pins');
        } else if (minPins + optional > budget) {
          warn('CFG-GPIO-002', `The buses need ${minPins} pins and the optional control pins ${optional} more; ${board.label} has about ${budget} free, so some control pins must stay unused.`, 'pins');
        }
      } else if (ceiling !== null && minPins > ceiling) {
        err('CFG-GPIO-001', `The buses need at least ${minPins} pins but a ${target.label} has ${ceiling} GPIO in all.`, 'pins');
      } else if (target && ceiling !== null) {
        info('CFG-GPIO-003', `The free pins of this board are not in the catalogue; the selection needs ${minPins} to ${minPins + optional}.`, 'pins');
      }
    }

    // -- tested sets and known vulnerabilities ------------------------------------------------------------------
    // A selection belongs to a tested set when every component is at that set's version (a subset of the set is
    // fine) and the set passed on this MCU family, target and RTOS. Drivers that did not resolve rule it out.
    const unresolved = (image.drivers || []).length !== chosen.length;
    const portName = family && family.port ? family.port.name : null;
    const tested = (catalog.tested_sets || []).filter((set) =>
      kbRelease && !unresolved && set.kb_core === kbRelease.version &&
      chosen.every((c) => set.drivers[c.d.name] === c.v.version) &&
      (!portRelease || set.port[portName] === portRelease.version) &&
      set.configs.some((cfg) => cfg.mcu === mcu.family && cfg.target === mcu.target && cfg.rtos === image.rtos && cfg.result === 'pass'));
    if (tested.length) info('CFG-SET-002', `This combination is in tested set ${tested.map((s) => s.id).join(', ')}.`, 'tested');
    else warn('CFG-SET-001', 'Untested combination: no tested version set holds exactly these versions on this MCU, target and RTOS.', 'tested');
    const components = [];
    if (kbRelease) components.push({ name: 'kb-core', osv: kbRelease.osv });
    if (portRelease) components.push({ name: `port-${portName}`, osv: portRelease.osv });
    for (const c of chosen) components.push({ name: c.d.name, osv: c.v.osv });
    const unscanned = [];
    for (const comp of components) {
      if (!comp.osv) { unscanned.push(comp.name); continue; }
      for (const f of comp.osv.findings) {
        add(f.severity === 'high' || f.severity === 'critical' ? 'error' : 'warning', 'CFG-CVE-001',
          `${comp.name}: ${f.id} (${f.severity}) ${f.summary}${f.fixed_in ? `; fixed in ${f.fixed_in}` : ''}.`, comp.name);
      }
    }
    if (unscanned.length) info('CFG-CVE-002', `Not scanned for known vulnerabilities yet: ${unscanned.join(', ')}.`, 'cve');

    // -- result ------------------------------------------------------------------------------------------------
    const parts = ['utils', 'rtos', 'interfaces'];
    if (chosen.length) parts.push('handlers');
    const resolvedBuses = {};
    for (const id of Object.keys(buses).sort()) {
      const b = buses[id];
      resolvedBuses[id] = { kind: b.kind, via: [...b.vias].sort(), model: b.model, devices: b.devices.map((c) => c.d.name).sort() };
      if (b.addresses) resolvedBuses[id].addresses = b.addresses;
    }
    const rank = { error: 0, warning: 1, info: 2 };
    findings.sort((a, b) => rank[a.severity] - rank[b.severity] || (a.code < b.code ? -1 : a.code > b.code ? 1 : 0) ||
      (String(a.subject) < String(b.subject) ? -1 : String(a.subject) > String(b.subject) ? 1 : 0));
    const errors = findings.filter((f) => f.severity === 'error');
    return {
      ok: errors.length === 0,
      findings,
      errors: errors.map((f) => f.code),
      warnings: findings.filter((f) => f.severity === 'warning').map((f) => f.code),
      infos: findings.filter((f) => f.severity === 'info').map((f) => f.code),
      resolved: {
        kb_mcu: family ? family.kb_mcu : null,
        kb_rtos: rtos ? rtos.kb_rtos : null,
        parts,
        drivers: chosen.map((c) => c.d.name).sort(),
        buses: resolvedBuses,
        pins: { min: minPins, optional, budget },
        tested_sets: tested.map((s) => s.id),
      },
      context: { family, target, board, rtos, kbRelease, portRelease, portInTree, chosen, components, unscanned },
    };
  }

  KB.resolver = { resolve, latestVersion, assignAddresses, MIRRORS };
})(typeof globalThis !== 'undefined' ? globalThis : this);
