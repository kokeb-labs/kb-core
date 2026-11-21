// The kb resolver for the configurator site: a pure function from (catalogue, image) to a lock or a list of
// structured errors, and the files a resolved image produces. Same specification as tools/kb/resolver.py and
// tools/kb/emit.py, rule by rule; both pass tests/resolver/vectors (ADR-0014). Specification:
// docs/design/assembler.md. No dependencies, no I/O, no clock.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors

const VERSION_RE = /^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.(0|[1-9][0-9]*))?$/;
const IMAGE_KEYS = ["buses", "drivers", "image", "kb_core", "mcu", "rtos", "schema"];
const NAME_RE = /^[a-z0-9][a-z0-9-]{0,62}$/;
const BUS_RE = /^[a-z][a-z0-9_]{0,31}$/;
const BUS_KIND_RE = /^(i2c|spi|uart|can|adc|pio|gpio)[0-9]*$/;
const BUS_KINDS = ["adc", "can", "canopen", "gpio", "i2c", "modbus", "onewire", "pio", "spi", "uart"];
const MODELS = ["guarded", "owner"];
const PART_ORDER = ["utils", "rtos", "interfaces", "handlers"];
const DEFAULT_IMAGE = "kb-image";

export const LOCK_FILE = "kb-image.lock.yml";
export const PRESETS_FILE = "CMakePresets.json";
export const CMAKE_FILE = "kb_image.cmake";
export const FETCH_FILE = "kb-fetch.sh";

// --- versions ---------------------------------------------------------------------------------------------------

export function parseVersion(text) {
  if (typeof text !== "string") return null;
  const m = VERSION_RE.exec(text);
  if (m === null) return null;
  const rc = m[5] === undefined ? Number.POSITIVE_INFINITY : Number(m[5]);
  return [Number(m[1]), Number(m[2]), Number(m[3]), rc];
}

export function compareVersions(a, b) {
  const ka = parseVersion(a);
  const kb = parseVersion(b);
  if (ka === null || kb === null) throw new Error(`not a version: ${a} or ${b}`);
  for (let i = 0; i < 4; i += 1) {
    if (ka[i] !== kb[i]) return ka[i] > kb[i] ? 1 : -1;
  }
  return 0;
}

// --- helpers ----------------------------------------------------------------------------------------------------

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const isStr = (v) => typeof v === "string";
const isInt = (v) => typeof v === "number" && Number.isInteger(v);
const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const cmpStr = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const sortedKeys = (o) => Object.keys(o).sort(cmpStr);

class Collector {
  constructor() {
    this.errors = [];
    this.warnings = [];
  }
  error(code, subject, message) {
    this.errors.push({ code, subject, message });
  }
  warn(code, subject, message) {
    this.warnings.push({ code, subject, message });
  }
}

function unknownKeys(c, obj, allowed, prefix) {
  for (const key of sortedKeys(obj)) {
    if (!allowed.includes(key)) c.error("KB-RES-022", `${prefix}${key}`, `unknown field '${prefix}${key}'`);
  }
}

// Phase 1: the image's shape. Returns the normalised driver list [[name, version or null]].
function checkShape(image, c) {
  if (!isObj(image)) {
    c.error("KB-RES-001", "$", "the image must be a mapping");
    return [];
  }
  unknownKeys(c, image, IMAGE_KEYS, "");
  if (!has(image, "schema") || !isInt(image.schema)) {
    c.error("KB-RES-001", "schema", "schema must be the integer 1");
  } else if (image.schema !== 1) {
    c.error("KB-RES-021", "schema", "image schema is not supported (expected 1)");
  }
  if (has(image, "image") && !(isStr(image.image) && NAME_RE.test(image.image))) {
    c.error("KB-RES-001", "image", "image must be a lower-case name: [a-z0-9][a-z0-9-]*, at most 63 characters");
  }
  const kb = image.kb_core;
  if (!isObj(kb)) {
    c.error("KB-RES-001", "kb_core", "kb_core must be a mapping with a version");
  } else {
    unknownKeys(c, kb, ["version"], "kb_core.");
    if (!isStr(kb.version)) c.error("KB-RES-001", "kb_core.version", "kb_core.version must be a string");
  }
  const mcu = image.mcu;
  if (!isObj(mcu)) {
    c.error("KB-RES-001", "mcu", "mcu must be a mapping with a family");
  } else {
    unknownKeys(c, mcu, ["family", "port", "target"], "mcu.");
    if (!isStr(mcu.family)) c.error("KB-RES-001", "mcu.family", "mcu.family must be a string");
    if (has(mcu, "target") && !isStr(mcu.target)) c.error("KB-RES-001", "mcu.target", "mcu.target must be a string");
    if (has(mcu, "port")) {
      const port = mcu.port;
      if (!isObj(port)) {
        c.error("KB-RES-001", "mcu.port", "mcu.port must be a mapping");
      } else {
        unknownKeys(c, port, ["version"], "mcu.port.");
        if (has(port, "version") && !isStr(port.version)) {
          c.error("KB-RES-001", "mcu.port.version", "mcu.port.version must be a string");
        }
      }
    }
  }
  if (has(image, "rtos") && !isStr(image.rtos)) c.error("KB-RES-001", "rtos", "rtos must be a string");
  const drivers = [];
  if (has(image, "drivers")) {
    const lst = image.drivers;
    if (!Array.isArray(lst)) {
      c.error("KB-RES-001", "drivers", "drivers must be a list");
    } else {
      lst.forEach((item, i) => {
        const where = `drivers[${i}]`;
        if (isStr(item)) {
          drivers.push([item, null]);
        } else if (isObj(item)) {
          unknownKeys(c, item, ["name", "version"], `${where}.`);
          let ok = true;
          if (!isStr(item.name)) {
            c.error("KB-RES-001", `${where}.name`, `${where}.name must be a string`);
            ok = false;
          }
          if (has(item, "version") && !isStr(item.version)) {
            c.error("KB-RES-001", `${where}.version`, `${where}.version must be a string`);
            ok = false;
          }
          if (ok) drivers.push([item.name, has(item, "version") ? item.version : null]);
        } else {
          c.error("KB-RES-001", where, `${where} must be a driver name or a mapping`);
        }
      });
    }
  }
  if (has(image, "buses")) {
    const buses = image.buses;
    if (!isObj(buses)) {
      c.error("KB-RES-001", "buses", "buses must be a mapping of bus name to bus");
    } else {
      for (const key of sortedKeys(buses)) {
        const where = `buses.${key}`;
        const bus = buses[key];
        if (!BUS_RE.test(key)) c.error("KB-RES-001", where, "a bus name is [a-z][a-z0-9_]*, at most 32 characters");
        if (!isObj(bus)) {
          c.error("KB-RES-001", where, `${where} must be a mapping`);
          continue;
        }
        unknownKeys(c, bus, ["devices", "kind", "model"], `${where}.`);
        if (!isStr(bus.model)) c.error("KB-RES-001", `${where}.model`, `${where}.model must be a string`);
        if (has(bus, "kind") && !isStr(bus.kind)) c.error("KB-RES-001", `${where}.kind`, `${where}.kind must be a string`);
        if (!Array.isArray(bus.devices)) {
          c.error("KB-RES-001", `${where}.devices`, `${where}.devices must be a list`);
        } else {
          bus.devices.forEach((dev, j) => {
            if (!isStr(dev)) c.error("KB-RES-001", `${where}.devices[${j}]`, "a device is a driver name");
          });
        }
      }
    }
  }
  return drivers;
}

function find(items, key, value) {
  for (const item of items) {
    if (item[key] === value) return item;
  }
  return null;
}

function newest(versions, kind) {
  let best = null;
  for (const v of versions) {
    if (v.kind !== kind) continue;
    if (best === null || compareVersions(v.version, best.version) > 0) best = v;
  }
  return best;
}

function defaultVersion(versions, pinned) {
  if (pinned !== null && pinned !== undefined) {
    const found = find(versions, "version", pinned);
    if (found !== null) return found;
  }
  return newest(versions, "release") || newest(versions, "rc");
}

const byCodeSubject = (a, b) => cmpStr(a.code, b.code) || cmpStr(a.subject, b.subject);

function result(c, lock) {
  return {
    ok: c.errors.length === 0,
    errors: [...c.errors].sort(byCodeSubject),
    warnings: [...c.warnings].sort(byCodeSubject),
    lock,
  };
}

/** Resolve `image` against `catalog`: {ok, errors, warnings, lock}; lock is null when there is an error. */
export function resolve(catalog, image) {
  const c = new Collector();
  if (!isObj(catalog) || catalog.schema !== 1) {
    c.error("KB-RES-030", "catalog", "the catalogue must have schema 1");
    return result(c, null);
  }
  const driversReq = checkShape(image, c);
  if (c.errors.length) return result(c, null);

  // kb-core
  const kbReq = image.kb_core.version;
  const kb = find(catalog.kb_core.versions, "version", kbReq);
  if (kb === null) {
    c.error("KB-RES-002", "kb_core.version", `kb-core ${kbReq} is not in the catalogue`);
  } else if (kb.kind === "rc") {
    c.warn("KB-RES-W01", "kb-core", `kb-core ${kbReq} is a release candidate`);
  } else if (kb.kind === "dev") {
    c.warn("KB-RES-W03", "kb-core", `kb-core ${kbReq} at ${kb.commit.slice(0, 12)} is not a release`);
  }

  // MCU family, target and port
  const famReq = image.mcu.family;
  let mcu = find(catalog.mcus, "family", famReq);
  let target = null;
  if (mcu === null) {
    c.error("KB-RES-003", "mcu.family", `MCU family '${famReq}' is not in the catalogue`);
  } else if (mcu.status !== "supported" || mcu.port === null || mcu.port === undefined) {
    c.error("KB-RES-004", "mcu.family", `MCU family '${famReq}' has no port yet`);
    mcu = null;
  } else {
    target = has(image.mcu, "target") ? image.mcu.target : mcu.targets[0];
    if (!mcu.targets.includes(target)) {
      c.error("KB-RES-005", "mcu.target", `'${target}' is not a target of ${famReq}: ${mcu.targets.join(", ")}`);
    }
  }

  // RTOS
  let rtosName = has(image, "rtos") ? image.rtos : null;
  if (rtosName === null && mcu !== null) rtosName = mcu.default_rtos;
  if (rtosName !== null) {
    const rtos = find(catalog.rtos, "name", rtosName);
    if (rtos === null) {
      c.error("KB-RES-006", "rtos", `RTOS '${rtosName}' is not in the catalogue`);
    } else if (rtos.status !== "supported") {
      c.error("KB-RES-007", "rtos", `the ${rtosName} backend is not implemented yet`);
    } else if (mcu !== null && !mcu.rtos.includes(rtosName)) {
      c.error("KB-RES-008", "rtos", `${famReq} supports ${mcu.rtos.join(", ")}, not ${rtosName}`);
    }
  }

  // Port
  let portLock = null;
  if (mcu !== null) {
    const port = mcu.port;
    const portReq = has(image.mcu, "port") && has(image.mcu.port, "version") ? image.mcu.port.version : null;
    if (port.versions && port.versions.length) {
      const chosen = portReq === null ? defaultVersion(port.versions, null) : find(port.versions, "version", portReq);
      if (chosen === null) {
        c.error("KB-RES-020", "mcu.port.version", `port-${port.name} has no version ${portReq}`);
      } else {
        if (chosen.kind === "rc") {
          c.warn("KB-RES-W01", `port-${port.name}`, `port-${port.name} ${chosen.version} is a release candidate`);
        }
        portLock = {
          name: port.name, in_tree: false, repo: port.repo, version: chosen.version,
          tag: chosen.tag, commit: chosen.commit, kind: chosen.kind,
        };
      }
    } else {
      if (portReq !== null && portReq !== kbReq) {
        c.error("KB-RES-020", "mcu.port.version",
          `the ${port.name} port is inside kb-core and has kb-core's version ${kbReq}, not ${portReq}`);
      }
      if (kb !== null) portLock = { name: port.name, in_tree: true, path: port.path, commit: kb.commit };
    }
  }

  // Drivers
  const selected = new Map();
  const listed = new Set();
  for (const [name, verReq] of driversReq) {
    if (listed.has(name)) {
      c.error("KB-RES-010", name, `driver '${name}' is listed twice`);
      continue;
    }
    listed.add(name);
    const drv = find(catalog.drivers, "name", name);
    if (drv === null) {
      c.error("KB-RES-009", name, `driver '${name}' is not in the catalogue`);
      continue;
    }
    if (verReq !== null && parseVersion(verReq) === null) {
      c.error("KB-RES-012", name, `'${verReq}' is not X.Y.Z or X.Y.Z-rc.N`);
      continue;
    }
    const ver = verReq === null ? defaultVersion(drv.versions, drv.pinned) : find(drv.versions, "version", verReq);
    if (ver === null) {
      c.error("KB-RES-011", name, `driver '${name}' has no version ${verReq !== null ? verReq : "(any)"}`);
      continue;
    }
    if (ver.kind === "rc") c.warn("KB-RES-W01", name, `${name} ${ver.version} is a release candidate`);
    if (mcu !== null && !ver.mcus.includes("any") && !ver.mcus.includes(famReq)) {
      let need = ver.mcus.join(", ");
      if (ver.peripheral) need = `${need} (${ver.peripheral})`;
      c.error("KB-RES-013", name, `${name} works only on ${need}, not on ${famReq}`);
    }
    const kmin = ver.kb_core_min;
    if (kb !== null && kmin !== null && kmin !== undefined && compareVersions(kmin, kb.version) > 0) {
      c.error("KB-RES-014", name, `${name} ${ver.version} needs kb-core ${kmin} or newer, not ${kb.version}`);
    }
    selected.set(name, [drv, ver]);
  }

  // Buses and thread models (ADR-0009)
  const busLock = [];
  const onBus = new Map();
  const buses = has(image, "buses") ? image.buses : {};
  for (const key of sortedKeys(buses)) {
    const bus = buses[key];
    const model = bus.model;
    if (!MODELS.includes(model)) {
      c.error("KB-RES-015", `buses.${key}.model`, `thread model '${model}' is not owner or guarded`);
    }
    let kind = has(bus, "kind") ? bus.kind : null;
    if (kind === null) {
      const m = BUS_KIND_RE.exec(key);
      kind = m ? m[1] : null;
    } else if (!BUS_KINDS.includes(kind)) {
      c.error("KB-RES-023", `buses.${key}.kind`, `bus kind '${kind}' is not one of ${BUS_KINDS.join(", ")}`);
      kind = null;
    }
    const seen = new Set();
    for (const dev of bus.devices) {
      const subject = `${key}/${dev}`;
      if (seen.has(dev)) {
        c.error("KB-RES-010", subject, `device '${dev}' is listed twice on bus ${key}`);
        continue;
      }
      seen.add(dev);
      if (!listed.has(dev)) {
        c.error("KB-RES-016", subject, `device '${dev}' on bus ${key} is not in drivers`);
        continue;
      }
      if (!onBus.has(dev)) onBus.set(dev, []);
      onBus.get(dev).push(key);
      if (!selected.has(dev)) continue;
      const ver = selected.get(dev)[1];
      if (ver.requires_owner && model === "guarded") {
        c.error("KB-RES-018", subject, `${dev} requires the owner-task model; bus ${key} is guarded`);
      }
      if (kind !== null && !ver.bus.includes(kind)) {
        c.error("KB-RES-019", subject, `${dev} ${ver.version} supports ${ver.bus.join(", ")}, not ${kind}`);
      }
    }
    busLock.push({ name: key, model, kind, devices: [...seen].sort(cmpStr) });
  }
  for (const dev of [...onBus.keys()].sort(cmpStr)) {
    if (onBus.get(dev).length > 1) {
      c.error("KB-RES-017", dev, `device '${dev}' is declared on buses ${onBus.get(dev).join(", ")}`);
    }
  }

  if (c.errors.length) return result(c, null);

  const parts = PART_ORDER.filter((p) => catalog.parts.includes(p) && (p !== "handlers" || selected.size > 0));
  const cmake = { KB_MCU: famReq.toUpperCase(), KB_RTOS: rtosName.toUpperCase(), KB_PARTS: parts.join(";") };
  const driversLock = [];
  for (const name of [...selected.keys()].sort(cmpStr)) {
    const [drv, ver] = selected.get(name);
    cmake[`KB_ENABLE_${name.toUpperCase()}`] = "ON";
    driversLock.push({
      name, version: ver.version, repo: drv.repo, folder: drv.folder, tag: ver.tag, commit: ver.commit,
      kind: ver.kind, license: has(ver, "license") ? ver.license : "NOASSERTION",
      authors: has(ver, "authors") ? [...ver.authors] : [],
    });
  }
  const lock = {
    schema: 1,
    image: has(image, "image") ? image.image : DEFAULT_IMAGE,
    kb_core: {
      version: kb.version, repo: catalog.kb_core.repo, tag: kb.tag, commit: kb.commit, kind: kb.kind,
      license: catalog.kb_core.license, authors: [...catalog.kb_core.authors],
    },
    mcu: { family: famReq, target },
    rtos: rtosName,
    parts,
    ports: [portLock],
    drivers: driversLock,
    buses: busLock,
    cmake,
  };
  return result(c, lock);
}

// --- emitted files (tools/kb/emit.py) ---------------------------------------------------------------------------

function sortDeep(v) {
  if (Array.isArray(v)) return v.map(sortDeep);
  if (isObj(v)) {
    const out = {};
    for (const k of sortedKeys(v)) out[k] = sortDeep(v[k]);
    return out;
  }
  return v;
}

/** Sorted keys, two-space indent, one final newline: the bytes of Python's json.dumps(indent=2, sort_keys=True). */
export function canonicalJson(value) {
  return `${JSON.stringify(sortDeep(value), null, 2)}\n`;
}

export function lockText(lock) {
  return `# kb image lock (schema 1), written by kb resolve: do not edit.\n${canonicalJson(lock)}`;
}

function presetVariables(lock) {
  const out = { ...lock.cmake };
  for (const drv of lock.drivers) out[`KB_DRIVER_${drv.name.toUpperCase()}_DIR`] = `\${sourceDir}/external/drivers/${drv.folder}`;
  return out;
}

export function presetsText(lock) {
  const name = lock.image;
  return canonicalJson({
    version: 6,
    configurePresets: [{
      name,
      displayName: `kb image ${name}`,
      binaryDir: `\${sourceDir}/build/${name}`,
      cacheVariables: presetVariables(lock),
    }],
  });
}

const FIXED = ["KB_MCU", "KB_RTOS", "KB_PARTS"];
function cmakeOrder(a, b) {
  const ia = FIXED.includes(a) ? FIXED.indexOf(a) : FIXED.length;
  const ib = FIXED.includes(b) ? FIXED.indexOf(b) : FIXED.length;
  return ia - ib || cmpStr(ia === FIXED.length ? a : "", ib === FIXED.length ? b : "");
}

export function cmakeText(lock) {
  const kb = lock.kb_core;
  const lines = [
    `# kb image ${lock.image}: written by kb resolve from kb-image.yml (schema 1); do not edit.`,
    `# kb-core ${kb.version} at ${kb.commit}, MCU ${lock.mcu.family}/${lock.mcu.target}, RTOS ${lock.rtos}.`,
  ];
  for (const key of Object.keys(lock.cmake).sort(cmakeOrder)) {
    const value = lock.cmake[key];
    lines.push(value === "ON" ? `set(${key} ${value})` : `set(${key} "${value}")`);
  }
  // The drivers are where kb-fetch.sh puts them: external/drivers/, beside kb-core, not inside it.
  for (const drv of lock.drivers) {
    lines.push(`set(KB_DRIVER_${drv.name.toUpperCase()}_DIR "\${CMAKE_CURRENT_LIST_DIR}/external/drivers/${drv.folder}")`);
  }
  return `${lines.join("\n")}\n`;
}

function shellQuote(text) {
  return `'${(text === null || text === undefined ? "" : String(text)).replace(/'/g, "'\\''")}'`;
}

/** Where each part goes in the user's repository: submodules under external/ (one layout, ADR-0014). */
export function layout(lock) {
  const kb = lock.kb_core;
  const out = [{
    label: `kb-core ${kb.version}`, path: "external/kb-core", url: `https://github.com/${kb.repo}.git`,
    tag: kb.tag, commit: kb.commit,
  }];
  for (const port of lock.ports) {
    if (!port.in_tree) {
      out.push({
        label: `port-${port.name} ${port.version}`, path: `external/port-${port.name}`,
        url: `https://github.com/${port.repo}.git`, tag: port.tag, commit: port.commit,
      });
    }
  }
  for (const drv of lock.drivers) {
    out.push({
      label: `driver ${drv.name} ${drv.version}`, path: `external/drivers/${drv.folder}`,
      url: `https://github.com/${drv.repo}.git`, tag: drv.tag, commit: drv.commit,
    });
  }
  return out;
}

const FETCH_BODY = [
  "# submodules under external/, each checked out at its tag (a kb-core development revision: at its commit) and",
  "# verified against the commit in kb-image.lock.yml (ADR-0012, ADR-0014). A tag that moved is refused. No branch",
  "# is recorded in .gitmodules and nothing is fetched recursively. Rerunning it is safe. Written by kb resolve.",
  "#",
  "#   sh kb-fetch.sh            add or update every pin",
  "#   sh kb-fetch.sh --verify   only check that every submodule is at its pinned commit",
  "set -eu",
  "",
  "mode=add",
  'if [ "${1:-}" = "--verify" ]; then mode=verify; fi',
  'top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "kb-fetch: run this inside a git repository" >&2; exit 1; }',
  'cd "$top"',
  "",
  "die() {",
  "  printf 'kb-fetch: error: %s\\n' \"$*\" >&2",
  "  exit 1",
  "}",
  "",
  "# pin <path> <url> <tag, empty for a commit pin> <commit>",
  "pin() {",
  "  path=$1 url=$2 tag=$3 want=$4",
  '  if [ "$mode" = verify ]; then',
  '    [ -e "$path/.git" ] || die "$path is missing; run without --verify"',
  "  else",
  '    if [ ! -e "$path/.git" ]; then',
  '      git submodule add --quiet "$url" "$path" || die "cannot add $url"',
  "    fi",
  '    if [ -n "$tag" ]; then',
  '      git -C "$path" fetch --quiet origin "+refs/tags/$tag:refs/tags/$tag" || die "cannot fetch $tag of $url"',
  '      got=$(git -C "$path" rev-parse "refs/tags/$tag^{commit}")',
  '      [ "$got" = "$want" ] || die "$path: tag $tag points at $got, expected $want (the tag moved; it is not used)"',
  "    else",
  '      git -C "$path" fetch --quiet origin "$want" || die "cannot fetch commit $want of $url"',
  "    fi",
  '    git -C "$path" -c advice.detachedHead=false checkout --quiet "$want"',
  '    git config -f .gitmodules --unset "submodule.$path.branch" 2>/dev/null || true',
  '    git add .gitmodules "$path"',
  "  fi",
  '  head=$(git -C "$path" rev-parse HEAD)',
  '  [ "$head" = "$want" ] || die "$path is at $head, expected $want"',
  "  printf 'kb-fetch: %s at %s (%s)\\n' \"$path\" \"${tag:-commit}\" \"$want\"",
  "}",
  "",
];

export function fetchText(lock) {
  const lines = [
    "#!/bin/sh",
    `# kb-fetch.sh: adds kb-core, the MCU port and the drivers of the image "${lock.image}" to this git repository as`,
    ...FETCH_BODY,
  ];
  for (const part of layout(lock)) {
    lines.push(`# ${part.label}${part.tag ? "" : " (development revision, no tag yet)"}`);
    lines.push(`pin ${shellQuote(part.path)} ${shellQuote(part.url)} ${shellQuote(part.tag)} ${shellQuote(part.commit)}`);
  }
  lines.push("", 'if [ "$mode" = add ]; then', '  echo "kb-fetch: done; review with git status and commit the submodules"', "fi");
  return `${lines.join("\n")}\n`;
}

/** Every file of a resolved image, name to text. */
export function files(lock) {
  return {
    [CMAKE_FILE]: cmakeText(lock),
    [FETCH_FILE]: fetchText(lock),
    [LOCK_FILE]: lockText(lock),
    [PRESETS_FILE]: presetsText(lock),
  };
}

// --- upgrades (tools/kb/upgrade.py) -----------------------------------------------------------------------------

export const IMAGE_FILE = "kb-image.yml";
export const REPORT_FILE = "kb-upgrade.md";

/** Why a newer driver version is held back: the resolver code of the gate it fails, in check order. */
export const HELD_TEXT = {
  "KB-RES-013": "needs another MCU family",
  "KB-RES-014": "needs a newer kb-core",
  "KB-RES-018": "requires an owner-task bus",
  "KB-RES-019": "does not support the bus kind in use",
};

const WARNING_TEXT = {
  "KB-RES-W01": "release candidate",
  "KB-RES-W03": "kb-core development revision",
  "KB-UPG-W01": "breaking change",
  "KB-UPG-W02": "newer version held back",
  "KB-UPG-W03": "known vulnerabilities remain",
  "KB-UPG-W04": "lock commit differs from the catalogue",
};

const KINDS = ["release", "rc", "dev"];

// {component: [kind or null, commit or null]} of a lock, for comparing with the catalogue.
function lockFacts(lock) {
  const out = new Map();
  if (!isObj(lock)) return out;
  const fact = (e) => [KINDS.includes(e.kind) ? e.kind : null, isStr(e.commit) ? e.commit : null];
  if (isObj(lock.kb_core)) out.set("kb-core", fact(lock.kb_core));
  for (const port of Array.isArray(lock.ports) ? lock.ports : []) {
    if (isObj(port) && port.in_tree === false && isStr(port.name)) out.set(`port-${port.name}`, fact(port));
  }
  for (const d of Array.isArray(lock.drivers) ? lock.drivers : []) if (isObj(d) && isStr(d.name)) out.set(d.name, fact(d));
  return out;
}

const clone = (v) => JSON.parse(JSON.stringify(v));

function failPlan(c) {
  return { ok: false, errors: [...c.errors].sort(byCodeSubject), warnings: [...c.warnings].sort(byCodeSubject), rows: [], image: null, lock: null };
}

function lockDriverVersions(lock, c) {
  let ok = isObj(lock) && lock.schema === 1 && isObj(lock.kb_core) && isStr(lock.kb_core.version)
    && Array.isArray(lock.drivers) && Array.isArray(has(lock, "ports") ? lock.ports : []);
  if (ok) {
    for (const d of lock.drivers) if (!(isObj(d) && isStr(d.name) && isStr(d.version))) ok = false;
    for (const p of has(lock, "ports") ? lock.ports : []) if (!isObj(p) || (has(p, "version") && !isStr(p.version))) ok = false;
  }
  if (!ok) {
    c.error("KB-UPG-002", "lock", "the lock is not a kb lock (schema 1) with kb_core and driver versions");
    return null;
  }
  const out = new Map();
  for (const d of lock.drivers) out.set(d.name, d.version);
  return out;
}

// The image with every version taken from the lock, so "current" means what the project builds today.
function pinCurrent(image, lock, c) {
  if (lock === null || lock === undefined) return image;
  const locked = lockDriverVersions(lock, c);
  if (locked === null) return image;
  const shapeOk = isObj(image) && isObj(image.kb_core) && isObj(image.mcu) && Array.isArray(has(image, "drivers") ? image.drivers : []);
  if (!shapeOk) return image;
  const cur = clone(image);
  const kbVer = lock.kb_core.version;
  if (isStr(cur.kb_core.version) && cur.kb_core.version !== kbVer) {
    c.error("KB-UPG-002", "kb_core.version", `the image names kb-core ${cur.kb_core.version}, the lock ${kbVer}`);
  }
  cur.kb_core.version = kbVer;
  for (const port of has(lock, "ports") ? lock.ports : []) {
    if (port.in_tree === false && isStr(port.version)) {
      const have = cur.mcu.port;
      if (isObj(have) && isStr(have.version) && have.version !== port.version) {
        c.error("KB-UPG-002", "mcu.port.version", `the image names port ${have.version}, the lock ${port.version}`);
      }
      cur.mcu.port = { version: port.version };
    }
  }
  const seen = new Set();
  const drivers = [];
  for (const item of has(cur, "drivers") ? cur.drivers : []) {
    const name = isStr(item) ? item : isObj(item) ? item.name : null;
    if (!isStr(name)) {
      drivers.push(item);
      continue;
    }
    seen.add(name);
    if (!locked.has(name)) {
      c.error("KB-UPG-002", name, `driver '${name}' is in the image but not in the lock`);
      drivers.push(item);
      continue;
    }
    if (isObj(item) && isStr(item.version) && item.version !== locked.get(name)) {
      c.error("KB-UPG-002", name, `the image names ${name} ${item.version}, the lock ${locked.get(name)}`);
    }
    drivers.push({ name, version: locked.get(name) });
  }
  for (const name of [...locked.keys()].filter((n) => !seen.has(n)).sort(cmpStr)) {
    c.error("KB-UPG-002", name, `driver '${name}' is in the lock but not in the image`);
  }
  if (has(cur, "drivers")) cur.drivers = drivers;
  return cur;
}

function findVersion(items, version) {
  for (const v of items) if (v.version === version) return v;
  return null;
}

// [chosen version entry, held [{version, code}]], or [null, []] after an error.
function pick(versions, current, allowRc, forced, gate, subject, c) {
  const cur = findVersion(versions, current.version);
  if (forced !== undefined && forced !== null) {
    const entry = parseVersion(forced) !== null ? findVersion(versions, forced) : null;
    if (entry === null) {
      c.error("KB-UPG-004", subject, `${subject} has no version ${forced} in the catalogue`);
      return [null, []];
    }
    if (compareVersions(entry.version, current.version) < 0) {
      c.error("KB-UPG-005", subject, `${subject} ${forced} is older than the current ${current.version}`);
      return [null, []];
    }
    const g = gate(entry);
    if (g !== null) {
      c.error(g, subject, `${subject} ${forced}: ${HELD_TEXT[g]}`);
      return [null, []];
    }
    return [entry, []];
  }
  const kinds = allowRc ? ["release", "rc"] : ["release"];
  const floor = current.kind === "dev" ? 0 : 1; // a release of the same version replaces a dev revision
  const cands = versions.filter((v) => kinds.includes(v.kind) && compareVersions(v.version, current.version) >= floor)
    .sort((a, b) => compareVersions(b.version, a.version));
  const held = [];
  for (const v of cands) {
    const g = gate(v);
    if (g === null) return [v, held];
    held.push({ version: v.version, code: g });
  }
  return [cur, held];
}

function breaking(a, b) {
  if (a === b) return false;
  const ka = parseVersion(a);
  const kb = parseVersion(b);
  if (ka[0] !== kb[0]) return true;
  if (ka[0] === 0) {
    if (ka[1] !== kb[1]) return true;
    if (ka[1] === 0 && ka[2] !== kb[2]) return true;
  }
  return false;
}

function change(a, b) {
  if (a === b) return "prerelease"; // same X.Y.Z[-rc.N], only the kind changed (a dev revision replaced by its release)
  const ka = parseVersion(a);
  const kb = parseVersion(b);
  const names = ["major", "minor", "patch"];
  for (let i = 0; i < 3; i += 1) if (ka[i] !== kb[i]) return names[i];
  return "prerelease";
}

function osvIds(entry) {
  const osv = entry ? entry.osv : null;
  if (!isObj(osv)) return null;
  const ids = new Set();
  const findings = Array.isArray(osv.findings) ? osv.findings : [];
  for (const f of findings) if (isObj(f) && isStr(f.id)) ids.add(f.id);
  return [...ids].sort(cmpStr);
}

function newestVersion(versions, kind) {
  let best = null;
  for (const v of versions) if (v.kind === kind && (best === null || compareVersions(v.version, best.version) > 0)) best = v;
  return best;
}

function upgradeRow(name, kind, repo, versions, cur, chosen, held, relocked = false) {
  const nr = newestVersion(versions, "release");
  let nrc = newestVersion(versions, "rc");
  if (nrc !== null && nr !== null && compareVersions(nrc.version, nr.version) < 0) nrc = null;
  const osvFrom = osvIds(findVersion(versions, cur.version));
  const osvTo = osvIds(chosen);
  const fixed = osvTo !== null ? [...new Set(osvFrom || [])].filter((i) => !osvTo.includes(i)).sort(cmpStr) : [];
  const changed = chosen.version !== cur.version || chosen.kind !== cur.kind || relocked;
  const notes = changed && chosen.tag ? `https://github.com/${repo}/releases/tag/${chosen.tag}` : null;
  return {
    name,
    kind,
    from: cur.version,
    from_kind: cur.kind,
    to: chosen.version,
    to_kind: chosen.kind,
    change: !changed ? "none" : chosen.version !== cur.version ? change(cur.version, chosen.version)
      : chosen.kind !== cur.kind ? "prerelease" : "commit",
    lock_differs: relocked,
    breaking: breaking(cur.version, chosen.version),
    newest_release: nr ? nr.version : null,
    newest_rc: nrc ? nrc.version : null,
    held,
    osv_from: osvFrom,
    osv_to: osvTo,
    fixed,
    notes,
  };
}

/**
 * Plan an upgrade of `image` (optionally with its current lock) to newer catalogued versions; the same rules as
 * tools/kb/upgrade.py. options: {lock, to: {component: version}, allowRc}. Returns {ok, errors, warnings, rows,
 * image, lock}.
 */
export function plan(catalog, image, options = {}) {
  const c = new Collector();
  const to = isObj(options.to) ? options.to : {};
  const allowRc = options.allowRc === true;
  const current = pinCurrent(image, options.lock, c);
  if (c.errors.length) return failPlan(c);
  const res = resolve(catalog, current);
  if (!res.ok) {
    c.error("KB-UPG-001", "image", "the current image does not resolve; fix it with kb resolve first");
    c.errors.push(...res.errors);
    return failPlan(c);
  }
  const cur = res.lock;
  const facts = lockFacts(options.lock);
  // The current {version, kind} and whether the lock's commit differs from the catalogue's (KB-UPG-W04).
  const currentOf = (name, entry) => {
    const [kind, commit] = facts.has(name) ? facts.get(name) : [null, null];
    const differs = (commit !== null && commit !== entry.commit) || (kind !== null && kind !== entry.kind);
    if (differs) {
      c.warn("KB-UPG-W04", name, `the lock pins ${name} ${entry.version} at ${(commit || "?").slice(0, 12)} (${kind || entry.kind}), the catalogue at ${entry.commit.slice(0, 12)} (${entry.kind})`);
    }
    return [{ version: entry.version, kind: differs && kind ? kind : entry.kind }, differs];
  };
  const fam = cur.mcu.family;
  const mcu = catalog.mcus.find((m) => m.family === fam);
  const portLock = cur.ports[0];
  const portName = `port-${portLock.name}`;
  const drivers = new Map(catalog.drivers.map((d) => [d.name, d]));
  const known = new Set(["kb-core", ...cur.drivers.map((d) => d.name)]);
  if (portLock.in_tree === false) known.add(portName);
  for (const key of sortedKeys(to)) {
    if (!known.has(key)) c.error("KB-UPG-003", key, key === portName ? `${key} is inside kb-core and moves with it` : `${key} is not in the image`);
  }
  if (c.errors.length) return failPlan(c);

  // kb-core first: the drivers' kb_core_min gates are checked against its target.
  const kbVersions = catalog.kb_core.versions;
  const [kbCur, kbRelock] = currentOf("kb-core", cur.kb_core);
  const none = () => null;
  const target = (k) => (has(to, k) ? to[k] : undefined);
  const [kbNew, kbHeld] = pick(kbVersions, kbCur, allowRc, target("kb-core"), none, "kb-core", c);
  const rows = [];
  if (kbNew !== null) rows.push(upgradeRow("kb-core", "kb-core", catalog.kb_core.repo, kbVersions, kbCur, kbNew, kbHeld, kbRelock));

  let portNew = null;
  if (portLock.in_tree === false) {
    const pv = mcu.port.versions;
    const [pCur, pRelock] = currentOf(portName, portLock);
    const [p, pHeld] = pick(pv, pCur, allowRc, target(portName), none, portName, c);
    portNew = p;
    if (p !== null) rows.push(upgradeRow(portName, "port", portLock.repo, pv, pCur, p, pHeld, pRelock));
  }

  const busesOf = new Map();
  for (const bus of cur.buses) {
    for (const dev of bus.devices) {
      if (!busesOf.has(dev)) busesOf.set(dev, []);
      busesOf.get(dev).push(bus);
    }
  }
  const chosen = new Map();
  for (const d of cur.drivers) {
    const name = d.name;
    const entry = drivers.get(name);
    const gate = (v) => {
      if (!v.mcus.includes("any") && !v.mcus.includes(fam)) return "KB-RES-013";
      const kmin = v.kb_core_min;
      if (kbNew !== null && kmin !== null && kmin !== undefined && compareVersions(kmin, kbNew.version) > 0) return "KB-RES-014";
      for (const bus of busesOf.get(name) || []) if (v.requires_owner && bus.model === "guarded") return "KB-RES-018";
      for (const bus of busesOf.get(name) || []) if (bus.kind !== null && !v.bus.includes(bus.kind)) return "KB-RES-019";
      return null;
    };
    const [dCur, dRelock] = currentOf(name, d);
    const [nv, held] = pick(entry.versions, dCur, allowRc, target(name), gate, name, c);
    if (nv !== null) {
      chosen.set(name, nv.version);
      rows.push(upgradeRow(name, "driver", d.repo, entry.versions, dCur, nv, held, dRelock));
    }
  }
  if (c.errors.length) return failPlan(c);

  const newImage = clone(current);
  newImage.kb_core = { version: kbNew.version };
  if (portNew !== null) newImage.mcu.port = { version: portNew.version };
  if (has(newImage, "drivers")) newImage.drivers = [...chosen.keys()].sort(cmpStr).map((n) => ({ name: n, version: chosen.get(n) }));
  const after = resolve(catalog, newImage);
  if (!after.ok) { // every gate above mirrors a resolver rule, so this means the two drifted apart
    c.errors.push(...after.errors);
    return failPlan(c);
  }
  c.warnings.push(...after.warnings);
  for (const r of rows) {
    if (r.breaking) c.warn("KB-UPG-W01", r.name, `${r.name} ${r.from} to ${r.to} is a breaking change`);
    if (r.held.length) c.warn("KB-UPG-W02", r.name, `${r.name} ${r.held[0].version} is held back: ${HELD_TEXT[r.held[0].code] || r.held[0].code}`);
    if (r.osv_to && r.osv_to.length) c.warn("KB-UPG-W03", r.name, `${r.name} ${r.to} has known vulnerabilities: ${r.osv_to.join(", ")}`);
  }
  return { ok: true, errors: [], warnings: [...c.warnings].sort(byCodeSubject), rows, image: newImage, lock: after.lock };
}

const q = (text) => JSON.stringify(text);
const flow = (pairs) => `{${pairs.map(([k, v]) => `${k}: ${v}`).join(", ")}}`;

/** A kb-image.yml in one fixed style: block keys, flow values, every string double-quoted. */
export function imageText(image) {
  const lines = [
    "# kb-image.yml: the selection of one firmware build (schema 1, docs/design/assembler.md).",
    "# Written by an upgrade (kb upgrade or the configurator); resolve it with: kb resolve kb-image.yml",
    `schema: ${image.schema}`,
  ];
  if (has(image, "image")) lines.push(`image: ${q(image.image)}`);
  const kb = image.kb_core;
  lines.push(`kb_core: ${has(kb, "version") ? flow([["version", q(kb.version)]]) : "{}"}`);
  const mcu = image.mcu;
  const pairs = [["family", q(mcu.family)]];
  if (has(mcu, "target")) pairs.push(["target", q(mcu.target)]);
  if (has(mcu, "port")) pairs.push(["port", has(mcu.port, "version") ? flow([["version", q(mcu.port.version)]]) : "{}"]);
  lines.push(`mcu: ${flow(pairs)}`);
  if (has(image, "rtos")) lines.push(`rtos: ${q(image.rtos)}`);
  if (has(image, "drivers")) {
    if (!image.drivers.length) lines.push("drivers: []");
    else {
      lines.push("drivers:");
      for (const item of image.drivers) {
        const d = isStr(item) ? { name: item } : item;
        const p = [["name", q(d.name)]];
        if (has(d, "version")) p.push(["version", q(d.version)]);
        lines.push(`  - ${flow(p)}`);
      }
    }
  }
  if (has(image, "buses")) {
    const keys = sortedKeys(image.buses);
    if (!keys.length) lines.push("buses: {}");
    else {
      lines.push("buses:");
      for (const key of keys) {
        const bus = image.buses[key];
        const p = [["model", q(bus.model)]];
        if (has(bus, "kind")) p.push(["kind", q(bus.kind)]);
        p.push(["devices", `[${bus.devices.map(q).join(", ")}]`]);
        lines.push(`  ${key}: ${flow(p)}`);
      }
    }
  }
  return `${lines.join("\n")}\n`;
}

const cell = (v) => (v === null || v === undefined ? "-" : String(v));
const vulns = (ids) => (ids === null ? "not scanned" : ids.length ? ids.join(", ") : "none");

/** The upgrade report (Markdown) of a successful plan. */
export function upgradeReport(result, allowRc = false) {
  const lock = result.lock;
  const rows = result.rows;
  const kbRow = rows[0];
  const changed = rows.filter((r) => r.change !== "none");
  const lines = [
    `# Upgrade report: ${lock.image}`,
    "",
    `kb-core ${kbRow.from} to ${kbRow.to}, MCU ${lock.mcu.family}/${lock.mcu.target}, RTOS ${lock.rtos}. Release candidates ${allowRc ? "allowed" : "not picked unless named"}.`,
    "",
    changed.length ? `${changed.length} of ${rows.length} component(s) change.` : "Every component is at its newest selectable version.",
    "",
    "| Component | From | To | Change | Breaking | Newest release | Newer rc | Known vulnerabilities |",
    "| --- | --- | --- | --- | --- | --- | --- | --- |",
  ];
  for (const r of rows) {
    const vul = r.change === "none" ? vulns(r.osv_from) : `${vulns(r.osv_from)} -> ${vulns(r.osv_to)}`;
    lines.push(`| ${r.name} | ${r.from} | ${r.to} | ${r.change} | ${r.breaking ? "yes" : "no"} | ${cell(r.newest_release)} | ${cell(r.newest_rc)} | ${vul} |`);
  }
  lines.push("", "## Breaking changes", "");
  const brk = rows.filter((r) => r.breaking);
  for (const r of brk) lines.push(`- ${r.name} ${r.from} -> ${r.to}: read its release notes and CHANGELOG before building: ${r.notes || "no release page"}`);
  if (!brk.length) lines.push("None by version number. Read each changed component's release notes all the same.");
  lines.push("", "## Held back", "");
  const held = rows.flatMap((r) => r.held.map((h) => [r, h]));
  for (const [r, h] of held) lines.push(`- ${r.name} ${h.version}: ${HELD_TEXT[h.code] || h.code} (${h.code})`);
  if (!held.length) lines.push("None.");
  lines.push("", "## Known vulnerabilities", "");
  const fixed = rows.flatMap((r) => r.fixed.map((i) => [r, i]));
  const left = rows.flatMap((r) => (r.osv_to || []).map((i) => [r, i]));
  const unscanned = rows.filter((r) => r.osv_to === null).map((r) => r.name);
  for (const [r, i] of fixed) lines.push(`- fixed by this upgrade: ${i} (${r.name})`);
  for (const [r, i] of left) lines.push(`- still present: ${i} (${r.name} ${r.to})`);
  if (!fixed.length && !left.length) lines.push("- none known in the scanned components");
  if (unscanned.length) lines.push(`- not scanned: ${unscanned.join(", ")}`);
  lines.push("", "## Warnings", "");
  for (const w of result.warnings) lines.push(`- ${w.code} ${w.subject}: ${WARNING_TEXT[w.code] || w.code}`);
  if (!result.warnings.length) lines.push("None.");
  lines.push(
    "",
    "## Next steps",
    "",
    "1. Review the changes to `kb-image.yml` and `kb-image.lock.yml` with `git diff`.",
    "2. Run `sh kb-fetch.sh` to move every submodule to its new tag, then build and run your tests.",
    "3. Check the compatibility of the new set: a tested version set, or an untested combination.",
  );
  return `${lines.join("\n")}\n`;
}

/** Every file an upgrade writes: the new image, its lock and build files, and the report. */
export function upgradeFiles(result, allowRc = false) {
  const out = files(result.lock);
  out[IMAGE_FILE] = imageText(result.image);
  out[REPORT_FILE] = upgradeReport(result, allowRc);
  return out;
}
