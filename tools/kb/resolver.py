# The kb resolver: a pure function from (catalogue, image) to a lock or a list of structured errors. The same
# specification is implemented in JavaScript by configurator/resolver.mjs; both pass tests/resolver/vectors
# (ADR-0014). Specification: docs/design/assembler.md. Keep the two files in step, rule by rule.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Resolve a kb image selection against a catalogue (pure, deterministic, no I/O)."""

import re

from . import semver

#: Error and warning codes with their one-line meaning (docs/design/assembler.md, "Codes").
CODES = {
    "KB-RES-001": "malformed image: a field is missing or has the wrong type",
    "KB-RES-002": "unknown kb-core version",
    "KB-RES-003": "unknown MCU family",
    "KB-RES-004": "the MCU family has no port yet",
    "KB-RES-005": "unknown target of the MCU family",
    "KB-RES-006": "unknown RTOS",
    "KB-RES-007": "the RTOS backend is planned but not implemented yet",
    "KB-RES-008": "the RTOS is not available on the MCU family",
    "KB-RES-009": "unknown driver",
    "KB-RES-010": "a driver or bus device is listed twice",
    "KB-RES-011": "the driver has no such version (or no version at all)",
    "KB-RES-012": "not a version: X.Y.Z or X.Y.Z-rc.N",
    "KB-RES-013": "MCU gate: the driver needs another MCU family",
    "KB-RES-014": "the driver version needs a newer kb-core (kb_core_min)",
    "KB-RES-015": "unknown thread model: owner or guarded",
    "KB-RES-016": "a bus device is not an enabled driver",
    "KB-RES-017": "a device is declared on more than one bus",
    "KB-RES-018": "the chip requires the owner-task model (kRequiresOwner) but its bus is guarded",
    "KB-RES-019": "the driver version does not support the bus kind",
    "KB-RES-020": "unknown port version",
    "KB-RES-021": "unsupported image schema",
    "KB-RES-022": "unknown field",
    "KB-RES-023": "unknown bus kind",
    "KB-RES-024": "the part needs a newer C++ standard than the project's language.standard",
    "KB-RES-025": "the part needs a newer compiler than the project's language.compilers",
    "KB-RES-030": "unsupported catalogue schema",
    "KB-RES-W01": "a release-candidate pin is selected",
    "KB-RES-W03": "kb-core is an unreleased development revision",
    "KB-RES-W04": "an older driver version is used: the default one needs a newer C++ standard or compiler",
}

IMAGE_KEYS = ("buses", "drivers", "image", "kb_core", "language", "mcu", "rtos", "schema")
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")
BUS_RE = re.compile(r"^[a-z][a-z0-9_]{0,31}$")
BUS_KIND_RE = re.compile(r"^(i2c|spi|uart|can|adc|pio|gpio)[0-9]*$")
BUS_KINDS = ("adc", "can", "canopen", "gpio", "i2c", "modbus", "onewire", "pio", "spi", "uart")
MODELS = ("guarded", "owner")
PART_ORDER = ("utils", "rtos", "dma", "interfaces", "handlers")
DEFAULT_IMAGE = "kb-image"

# The language floor (docs/language-floor.md). A catalogue entry without a `language` block is read as the registry
# default, profile KB-CPP20 of the organisation's standards registry (ci-tools tools/standards/standards.yml).
DEFAULT_LANGUAGE = {"standard": 20, "profile": "KB-CPP20",
                    "compilers": {"arm_gcc": "10", "clang": "13", "esp_idf": "5.0", "gcc": "10", "msvc": "19.30"}}
STANDARDS = (17, 20, 23, 26)
COMPILERS = ("arm_gcc", "clang", "esp_idf", "gcc", "msvc")
COMPILER_RE = re.compile(r"^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){0,2}$")


def _is_obj(v):
    return isinstance(v, dict)


def _is_str(v):
    return isinstance(v, str)


def _is_int(v):
    # JSON has one number type: 1 and 1.0 are the same value, as in JavaScript.
    if isinstance(v, bool):
        return False
    return isinstance(v, int) or (isinstance(v, float) and v.is_integer())


class _Collector:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, code, subject, message, **extra):
        self.errors.append({"code": code, "subject": subject, "message": message, **extra})

    def warn(self, code, subject, message, **extra):
        self.warnings.append({"code": code, "subject": subject, "message": message, **extra})


def _unknown_keys(c, obj, allowed, prefix):
    for key in sorted(k for k in obj if k not in allowed):
        c.error("KB-RES-022", f"{prefix}{key}", f"unknown field '{prefix}{key}'")


def _check_shape(image, c):
    """Phase 1: the image's shape. Returns the normalised driver list [(name, version or None)]."""
    if not _is_obj(image):
        c.error("KB-RES-001", "$", "the image must be a mapping")
        return []
    for key in image:
        if not _is_str(key):
            c.error("KB-RES-001", "$", "every field name must be a string")
            return []
    _unknown_keys(c, image, IMAGE_KEYS, "")
    if "schema" not in image or not _is_int(image["schema"]):
        c.error("KB-RES-001", "schema", "schema must be the integer 1")
    elif image["schema"] != 1:
        c.error("KB-RES-021", "schema", "image schema is not supported (expected 1)")
    if "image" in image and not (_is_str(image["image"]) and NAME_RE.match(image["image"])):
        c.error("KB-RES-001", "image", "image must be a lower-case name: [a-z0-9][a-z0-9-]*, at most 63 characters")
    kb = image.get("kb_core")
    if not _is_obj(kb):
        c.error("KB-RES-001", "kb_core", "kb_core must be a mapping with a version")
    else:
        _unknown_keys(c, kb, ("version",), "kb_core.")
        if not _is_str(kb.get("version")):
            c.error("KB-RES-001", "kb_core.version", "kb_core.version must be a string")
    mcu = image.get("mcu")
    if not _is_obj(mcu):
        c.error("KB-RES-001", "mcu", "mcu must be a mapping with a family")
    else:
        _unknown_keys(c, mcu, ("family", "port", "target"), "mcu.")
        if not _is_str(mcu.get("family")):
            c.error("KB-RES-001", "mcu.family", "mcu.family must be a string")
        if "target" in mcu and not _is_str(mcu["target"]):
            c.error("KB-RES-001", "mcu.target", "mcu.target must be a string")
        if "port" in mcu:
            port = mcu["port"]
            if not _is_obj(port):
                c.error("KB-RES-001", "mcu.port", "mcu.port must be a mapping")
            else:
                _unknown_keys(c, port, ("version",), "mcu.port.")
                if "version" in port and not _is_str(port["version"]):
                    c.error("KB-RES-001", "mcu.port.version", "mcu.port.version must be a string")
    if "rtos" in image and not _is_str(image["rtos"]):
        c.error("KB-RES-001", "rtos", "rtos must be a string")
    if "language" in image:
        _check_language_shape(image["language"], c)
    drivers = []
    if "drivers" in image:
        lst = image["drivers"]
        if not isinstance(lst, list):
            c.error("KB-RES-001", "drivers", "drivers must be a list")
        else:
            for i, item in enumerate(lst):
                where = f"drivers[{i}]"
                if _is_str(item):
                    drivers.append((item, None))
                elif _is_obj(item):
                    _unknown_keys(c, item, ("name", "version"), f"{where}.")
                    ok = True
                    if not _is_str(item.get("name")):
                        c.error("KB-RES-001", f"{where}.name", f"{where}.name must be a string")
                        ok = False
                    if "version" in item and not _is_str(item["version"]):
                        c.error("KB-RES-001", f"{where}.version", f"{where}.version must be a string")
                        ok = False
                    if ok:
                        drivers.append((item["name"], item.get("version")))
                else:
                    c.error("KB-RES-001", where, f"{where} must be a driver name or a mapping")
    if "buses" in image:
        buses = image["buses"]
        if not _is_obj(buses):
            c.error("KB-RES-001", "buses", "buses must be a mapping of bus name to bus")
        else:
            if not all(_is_str(k) for k in buses):
                c.error("KB-RES-001", "buses", "every bus name must be a string")
                return drivers
            for key in sorted(buses):
                where = f"buses.{key}"
                bus = buses[key]
                if not BUS_RE.match(key):
                    c.error("KB-RES-001", where, "a bus name is [a-z][a-z0-9_]*, at most 32 characters")
                if not _is_obj(bus):
                    c.error("KB-RES-001", where, f"{where} must be a mapping")
                    continue
                _unknown_keys(c, bus, ("devices", "kind", "model"), f"{where}.")
                if not _is_str(bus.get("model")):
                    c.error("KB-RES-001", f"{where}.model", f"{where}.model must be a string")
                if "kind" in bus and not _is_str(bus["kind"]):
                    c.error("KB-RES-001", f"{where}.kind", f"{where}.kind must be a string")
                devices = bus.get("devices")
                if not isinstance(devices, list):
                    c.error("KB-RES-001", f"{where}.devices", f"{where}.devices must be a list")
                else:
                    for j, dev in enumerate(devices):
                        if not _is_str(dev):
                            c.error("KB-RES-001", f"{where}.devices[{j}]", "a device is a driver name")
    return drivers


def _check_language_shape(lang, c):
    if not _is_obj(lang):
        c.error("KB-RES-001", "language", "language must be a mapping with a standard")
        return
    _unknown_keys(c, lang, ("compilers", "standard"), "language.")
    if not _is_int(lang.get("standard")) or int(lang["standard"]) not in STANDARDS:
        c.error("KB-RES-001", "language.standard",
                f"language.standard must be one of {', '.join(str(s) for s in STANDARDS)}")
    if "compilers" in lang:
        comps = lang["compilers"]
        if not _is_obj(comps):
            c.error("KB-RES-001", "language.compilers", "language.compilers must be a mapping of compiler to version")
            return
        _unknown_keys(c, comps, COMPILERS, "language.compilers.")
        for key in sorted(k for k in comps if k in COMPILERS):
            if not (_is_str(comps[key]) and COMPILER_RE.match(comps[key])):
                c.error("KB-RES-001", f"language.compilers.{key}",
                        f"language.compilers.{key} must be a quoted version such as \"12\" or \"12.2\"")


def _language(entry):
    """The language block of a catalogue entry, or the registry default when it has none."""
    lang = entry.get("language")
    if not _is_obj(lang):
        return DEFAULT_LANGUAGE
    return {"standard": int(lang.get("standard", 20)), "compilers": dict(lang.get("compilers") or {})}


def _numbers(text):
    return tuple(int(p) for p in text.split("."))


def misfit(entry, want):
    """None when `entry` builds with the project's declared language `want`, else (code, needs, has, raise)."""
    lang = _language(entry)
    if lang["standard"] > want["standard"]:
        return ("KB-RES-024", f"C++{lang['standard']}", f"C++{want['standard']}",
                f"language.standard to {lang['standard']}")
    for key in sorted(want.get("compilers", {})):
        need = lang["compilers"].get(key)
        have = want["compilers"][key]
        if need is not None and _numbers(need) > _numbers(have):
            return ("KB-RES-025", f"{key} {need} or newer", f"{key} {have}", f"language.compilers.{key} to {need}")
    return None


def _newest_fitting(versions, want):
    """The newest release whose floor the project meets, else the newest release candidate, else None."""
    fit = [v for v in versions if v.get("kind") != "dev" and misfit(v, want) is None]
    return _newest(fit, "release") or _newest(fit, "rc")


def _language_finding(c, subject, label, entry, want, versions):
    """Report a part above the project's language floor, naming the nearest version that fits (or none)."""
    bad = misfit(entry, want)
    if bad is None:
        return
    code, needs, has, lift = bad
    alt = _newest_fitting(versions, want)
    hint = (f"{label} {alt['version']} is the newest version that builds with {has}, or raise {lift}"
            if alt is not None else f"no version of {label} builds with {has}; raise {lift}")
    c.error(code, subject, f"{label} {entry['version']} needs {needs}; the project builds with {has}. {hint}",
            alternative=None if alt is None else alt["version"])


def wanted_language(image):
    """The image's declared language ({standard, compilers}), or None when it declares none."""
    lang = image.get("language") if _is_obj(image) else None
    if lang is None:
        return None
    return {"standard": int(lang["standard"]), "compilers": dict(lang.get("compilers") or {})}


def _find(items, key, value):
    for item in items:
        if item.get(key) == value:
            return item
    return None


def _newest(versions, kind):
    best = None
    for v in versions:
        if v.get("kind") != kind:
            continue
        if best is None or semver.compare(v["version"], best["version"]) > 0:
            best = v
    return best


def _default_version(versions, pinned):
    """The version used when the image names none: kb-core's pin, else the newest release, else the newest rc."""
    if pinned is not None:
        found = _find(versions, "version", pinned)
        if found is not None:
            return found
    return _newest(versions, "release") or _newest(versions, "rc")


def resolve(catalog, image):
    """Resolve ``image`` against ``catalog``.

    Returns ``{"ok", "errors", "warnings", "lock"}``. Errors and warnings are lists of ``{code, subject, message}``
    sorted by code then subject; ``lock`` is None when there is an error.
    """
    c = _Collector()
    if not _is_obj(catalog) or catalog.get("schema") != 1 or isinstance(catalog.get("schema"), bool):
        c.error("KB-RES-030", "catalog", "the catalogue must have schema 1")
        return _result(c, None)
    drivers_req = _check_shape(image, c)
    if c.errors:
        return _result(c, None)

    # kb-core
    kb_req = image["kb_core"]["version"]
    kb = _find(catalog["kb_core"]["versions"], "version", kb_req)
    if kb is None:
        c.error("KB-RES-002", "kb_core.version", f"kb-core {kb_req} is not in the catalogue")
    elif kb["kind"] == "rc":
        c.warn("KB-RES-W01", "kb-core", f"kb-core {kb_req} is a release candidate")
    elif kb["kind"] == "dev":
        c.warn("KB-RES-W03", "kb-core", f"kb-core {kb_req} at {kb['commit'][:12]} is not a release")
    want = wanted_language(image)
    if kb is not None and want is not None:
        _language_finding(c, "kb-core", "kb-core", kb, want, catalog["kb_core"]["versions"])

    # MCU family, target and port
    fam_req = image["mcu"]["family"]
    mcu = _find(catalog["mcus"], "family", fam_req)
    target = None
    if mcu is None:
        c.error("KB-RES-003", "mcu.family", f"MCU family '{fam_req}' is not in the catalogue")
    elif mcu["status"] != "supported" or mcu.get("port") is None:
        c.error("KB-RES-004", "mcu.family", f"MCU family '{fam_req}' has no port yet")
        mcu = None
    else:
        target = image["mcu"].get("target", mcu["targets"][0])
        if target not in mcu["targets"]:
            c.error("KB-RES-005", "mcu.target", f"'{target}' is not a target of {fam_req}: {', '.join(mcu['targets'])}")

    # RTOS
    rtos_name = image.get("rtos")
    if rtos_name is None and mcu is not None:
        rtos_name = mcu["default_rtos"]
    if rtos_name is not None:
        rtos = _find(catalog["rtos"], "name", rtos_name)
        if rtos is None:
            c.error("KB-RES-006", "rtos", f"RTOS '{rtos_name}' is not in the catalogue")
        elif rtos["status"] != "supported":
            c.error("KB-RES-007", "rtos", f"the {rtos_name} backend is not implemented yet")
        elif mcu is not None and rtos_name not in mcu["rtos"]:
            c.error("KB-RES-008", "rtos", f"{fam_req} supports {', '.join(mcu['rtos'])}, not {rtos_name}")

    # Port
    port_lock = None
    if mcu is not None:
        port = mcu["port"]
        port_req = image["mcu"].get("port", {}).get("version")
        if port.get("versions"):
            if port_req is None:
                chosen = _default_version(port["versions"], None)
            else:
                chosen = _find(port["versions"], "version", port_req)
            if chosen is None:
                c.error("KB-RES-020", "mcu.port.version", f"port-{port['name']} has no version {port_req}")
            else:
                if chosen["kind"] == "rc":
                    c.warn("KB-RES-W01", f"port-{port['name']}", f"port-{port['name']} {chosen['version']} is a release candidate")
                port_lock = {"name": port["name"], "in_tree": False, "repo": port["repo"], "version": chosen["version"],
                             "tag": chosen["tag"], "commit": chosen["commit"], "kind": chosen["kind"]}
        else:
            if port_req is not None and port_req != kb_req:
                c.error("KB-RES-020", "mcu.port.version",
                        f"the {port['name']} port is inside kb-core and has kb-core's version {kb_req}, not {port_req}")
            if kb is not None:
                port_lock = {"name": port["name"], "in_tree": True, "path": port["path"], "commit": kb["commit"]}

    # Drivers
    selected = {}
    listed = set()
    for name, ver_req in drivers_req:
        if name in listed:
            c.error("KB-RES-010", name, f"driver '{name}' is listed twice")
            continue
        listed.add(name)
        drv = _find(catalog["drivers"], "name", name)
        if drv is None:
            c.error("KB-RES-009", name, f"driver '{name}' is not in the catalogue")
            continue
        if ver_req is not None and semver.parse_version(ver_req) is None:
            c.error("KB-RES-012", name, f"'{ver_req}' is not X.Y.Z or X.Y.Z-rc.N")
            continue
        if ver_req is None:
            ver = _default_version(drv["versions"], drv.get("pinned"))
        else:
            ver = _find(drv["versions"], "version", ver_req)
        if ver is None:
            missing = ver_req if ver_req is not None else "(any)"
            c.error("KB-RES-011", name, f"driver '{name}' has no version {missing}")
            continue
        if want is not None and ver_req is None and misfit(ver, want) is not None:
            # No version named: use the newest one the project's language can build (docs/language-floor.md).
            alt = _newest_fitting(drv["versions"], want)
            if alt is not None:
                c.warn("KB-RES-W04", name, f"{name} {ver['version']} needs {misfit(ver, want)[1]}; "
                       f"{alt['version']} is the newest version that builds with {misfit(ver, want)[2]}, so it is used",
                       alternative=alt["version"])
                ver = alt
        if want is not None:
            _language_finding(c, name, name, ver, want, drv["versions"])
        if ver["kind"] == "rc":
            c.warn("KB-RES-W01", name, f"{name} {ver['version']} is a release candidate")
        if mcu is not None and "any" not in ver["mcus"] and fam_req not in ver["mcus"]:
            need = ", ".join(ver["mcus"])
            if ver.get("peripheral"):
                need = f"{need} ({ver['peripheral']})"
            c.error("KB-RES-013", name, f"{name} works only on {need}, not on {fam_req}")
        kmin = ver.get("kb_core_min")
        if kb is not None and kmin is not None and semver.compare(kmin, kb["version"]) > 0:
            c.error("KB-RES-014", name, f"{name} {ver['version']} needs kb-core {kmin} or newer, not {kb['version']}")
        selected[name] = (drv, ver)

    # Buses and thread models (ADR-0009)
    bus_lock = []
    on_bus = {}
    buses = image.get("buses", {})
    for key in sorted(buses):
        bus = buses[key]
        model = bus["model"]
        if model not in MODELS:
            c.error("KB-RES-015", f"buses.{key}.model", f"thread model '{model}' is not owner or guarded")
        kind = bus.get("kind")
        if kind is None:
            m = BUS_KIND_RE.match(key)
            kind = m.group(1) if m else None
        elif kind not in BUS_KINDS:
            c.error("KB-RES-023", f"buses.{key}.kind", f"bus kind '{kind}' is not one of {', '.join(BUS_KINDS)}")
            kind = None
        seen = set()
        for dev in bus["devices"]:
            subject = f"{key}/{dev}"
            if dev in seen:
                c.error("KB-RES-010", subject, f"device '{dev}' is listed twice on bus {key}")
                continue
            seen.add(dev)
            if dev not in listed:
                c.error("KB-RES-016", subject, f"device '{dev}' on bus {key} is not in drivers")
                continue
            on_bus.setdefault(dev, []).append(key)
            if dev not in selected:
                continue
            ver = selected[dev][1]
            if ver.get("requires_owner") and model == "guarded":
                c.error("KB-RES-018", subject, f"{dev} requires the owner-task model; bus {key} is guarded")
            if kind is not None and kind not in ver["bus"]:
                c.error("KB-RES-019", subject, f"{dev} {ver['version']} supports {', '.join(ver['bus'])}, not {kind}")
        bus_lock.append({"name": key, "model": model, "kind": kind, "devices": sorted(seen)})
    for dev in sorted(on_bus):
        if len(on_bus[dev]) > 1:
            c.error("KB-RES-017", dev, f"device '{dev}' is declared on buses {', '.join(on_bus[dev])}")

    if c.errors:
        return _result(c, None)

    parts = [p for p in PART_ORDER if p in catalog["parts"] and (p != "handlers" or selected)]
    cmake = {"KB_MCU": fam_req.upper(), "KB_RTOS": rtos_name.upper(), "KB_PARTS": ";".join(parts)}
    drivers_lock = []
    for name in sorted(selected):
        drv, ver = selected[name]
        cmake[f"KB_ENABLE_{name.upper()}"] = "ON"
        drivers_lock.append({"name": name, "version": ver["version"], "repo": drv["repo"], "folder": drv["folder"],
                             "tag": ver["tag"], "commit": ver["commit"], "kind": ver["kind"],
                             "license": ver.get("license", "NOASSERTION"), "authors": list(ver.get("authors", []))})
    lock = {
        "schema": 1,
        "image": image.get("image", DEFAULT_IMAGE),
        "kb_core": {"version": kb["version"], "repo": catalog["kb_core"]["repo"], "tag": kb["tag"],
                    "commit": kb["commit"], "kind": kb["kind"], "license": catalog["kb_core"]["license"],
                    "authors": list(catalog["kb_core"]["authors"])},
        "mcu": {"family": fam_req, "target": target},
        "rtos": rtos_name,
        "parts": parts,
        "ports": [port_lock],
        "drivers": drivers_lock,
        "buses": bus_lock,
        "cmake": cmake,
    }
    if want is not None:
        cmake["KB_CPP_STANDARD"] = str(want["standard"])
        compilers = {k: want["compilers"][k] for k in sorted(want["compilers"])}
        lock["language"] = {"standard": want["standard"], "compilers": compilers}
    return _result(c, lock)


def _key(entry):
    return (entry["code"], entry["subject"])


def _result(c, lock):
    return {"ok": not c.errors, "errors": sorted(c.errors, key=_key), "warnings": sorted(c.warnings, key=_key),
            "lock": lock}


AVAILABILITY_CODES = {
    "KB-RES-001": "the selection is malformed",
    "KB-RES-003": "unknown MCU family",
    "KB-RES-004": "no port for the MCU family yet",
    "KB-RES-005": "unknown target",
    "KB-RES-006": "unknown RTOS",
    "KB-RES-007": "the RTOS backend is not implemented yet",
    "KB-RES-008": "the RTOS is not available on the MCU family",
    "KB-RES-013": "the driver needs another MCU family",
    "KB-RES-014": "the driver needs a newer kb-core",
    "KB-RES-024": "the driver needs a newer C++ standard",
    "KB-RES-025": "the driver needs a newer compiler",
    "KB-RES-031": "the driver needs a peripheral the target does not offer",
    "KB-RES-032": "the driver needs a bus the target does not offer",
    "KB-RES-033": "the driver needs another RTOS",
}
# Codes availability() can return: the resolver's own where one exists, KB-RES-031 to 033 for the gates only it has.


def _requires_of(drv):
    """What a driver needs of a build: its `requires` block, else read from its newest version (older catalogues)."""
    if _is_obj(drv.get("requires")):
        return drv["requires"]
    versions = drv.get("versions") or []
    v = versions[0] if versions else {}
    return {"bus": v.get("bus") or [], "mcus": v.get("mcus") or ["any"], "peripheral": v.get("peripheral") or None,
            "rtos": [], "kb_core_min": v.get("kb_core_min") or None, "language": v.get("language")}


def availability(catalog, selection):
    """Which drivers a build with this selection can use, from the site catalogue alone (families, targets, rtos,
    drivers' ``requires``): ``{"available": [names], "unavailable": [{name, code, reason}]}``, both sorted by name.

    ``selection`` is ``{family, target?, rtos?, language?: {standard, compilers?}, kb_core?}``; a missing target, rtos
    or language is the family's first target, its default_rtos and its default_language. A selection that cannot
    build at all (unknown family, no port, unknown target or RTOS) makes every driver unavailable for that one
    reason. A driver that fails several gates reports the first of: MCU family, peripheral, bus, RTOS, kb-core,
    language. Same answer as availability() in configurator/resolver.mjs (shared vectors, ADR-0014).
    """
    drivers = sorted(catalog["drivers"], key=lambda d: d["name"])

    def block_all(code, reason):
        return {"available": [], "unavailable": [{"name": d["name"], "code": code, "reason": reason} for d in drivers]}

    sel = selection if _is_obj(selection) else {}
    fam = _find(catalog.get("families") or [], "id", sel.get("family"))
    if fam is None:
        return block_all("KB-RES-003", f"MCU family '{sel.get('family')}' is not in the catalogue")
    if fam["status"] != "supported" or fam.get("port") is None:
        return block_all("KB-RES-004", f"no port for {fam['label']} yet")
    target_id = sel.get("target")
    if target_id is None:
        target_id = fam["targets"][0]["id"]
    target = _find(fam["targets"], "id", target_id)
    if target is None:
        return block_all("KB-RES-005", f"'{target_id}' is not a target of {fam['id']}")
    rtos_id = sel.get("rtos")
    if rtos_id is None:
        rtos_id = fam["default_rtos"]
    rtos = _find(catalog.get("rtos") or [], "id", rtos_id)
    if rtos is None:
        return block_all("KB-RES-006", f"RTOS '{rtos_id}' is not in the catalogue")
    if rtos["status"] != "supported":
        return block_all("KB-RES-007", f"the {rtos_id} backend is not implemented yet")
    if rtos_id not in fam["rtos"]:
        return block_all("KB-RES-008", f"{fam['label']} supports {', '.join(fam['rtos'])}, not {rtos_id}")
    lang = sel["language"] if _is_obj(sel.get("language")) else (
        fam["default_language"] if _is_obj(fam.get("default_language")) else DEFAULT_LANGUAGE)
    if lang.get("standard") not in STANDARDS or isinstance(lang.get("standard"), bool):
        return block_all("KB-RES-001", f"language.standard must be one of {', '.join(str(s) for s in STANDARDS)}")
    want = {"standard": lang["standard"], "compilers": dict(lang.get("compilers") or {})}
    kinds = catalog.get("bus_kinds") or {}

    def offers(kind):
        if kind not in kinds:
            return True  # a bus kind the catalogue does not describe cannot be refused
        buses = target.get("buses") or {}
        instance = kinds[kind]["instance"]
        if instance not in buses:
            return False
        return buses[instance] is None or buses[instance] > 0

    available, unavailable = [], []
    for drv in drivers:
        req = _requires_of(drv)

        def fail(code, reason, name=drv["name"]):
            unavailable.append({"name": name, "code": code, "reason": reason})

        mcus = req.get("mcus") or ["any"]
        periph = req.get("peripheral")
        buses = req.get("bus") or []
        rtos_need = req.get("rtos") or []
        kmin = req.get("kb_core_min")
        bad = misfit({"language": req.get("language")}, want)
        if "any" not in mcus and fam["id"] not in mcus:
            fail("KB-RES-013", f"needs MCU family {' or '.join(mcus)}, not {fam['id']}")
        elif periph and periph not in (target.get("peripherals") or []):
            fail("KB-RES-031", f"needs the {periph.upper()} peripheral, which {target['label']} does not offer")
        elif buses and not any(offers(b) for b in buses):
            labels = [kinds[b]["label"] if b in kinds else b for b in buses]
            fail("KB-RES-032", f"needs {' or '.join(labels)}, which {target['label']} does not offer")
        elif rtos_need and rtos_id not in rtos_need:
            fail("KB-RES-033", f"needs {' or '.join(rtos_need)}, project uses {rtos_id}")
        elif kmin and sel.get("kb_core") and semver.compare(kmin, sel["kb_core"]) > 0:
            fail("KB-RES-014", f"needs kb-core {kmin} or newer, not {sel['kb_core']}")
        elif bad is not None:
            fail(bad[0], f"needs {bad[1]}, project is {bad[2]}")
        else:
            available.append(drv["name"])
    return {"available": available, "unavailable": unavailable}
