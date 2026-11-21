# The upgrade planner of `kb outdated` and `kb upgrade`: a pure function from (catalogue, image, lock, targets)
# to an upgrade plan, the new image and its lock. The same specification is implemented in JavaScript by
# configurator/resolver.mjs (plan, imageText, upgradeReport, upgradeFiles); both pass
# tests/resolver/vectors/upgrade (ADR-0014). Specification: docs/design/assembler.md, "Upgrades".
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Plan an upgrade of a kb image to newer catalogued versions (pure, deterministic, no I/O)."""

import copy
import json

from . import emit, resolver, semver

#: Upgrade codes with their one-line meaning (docs/design/assembler.md, "Upgrade codes").
CODES = {
    "KB-UPG-001": "the current image does not resolve against this catalogue",
    "KB-UPG-002": "the lock is not a lock of this image",
    "KB-UPG-003": "a target names a component that is not in the image",
    "KB-UPG-004": "a target names a version that is not in the catalogue",
    "KB-UPG-005": "a target is older than the current version (a downgrade)",
    "KB-UPG-W01": "a breaking change: a new major version, or a new minor (or patch) before 1.0",
    "KB-UPG-W02": "a newer version exists but a gate holds it back",
    "KB-UPG-W03": "known vulnerabilities remain after the upgrade",
    "KB-UPG-W04": "the lock pins a different commit (or kind) than the catalogue has for that version",
}

IMAGE_FILE = "kb-image.yml"
REPORT_FILE = "kb-upgrade.md"

#: Why a newer driver version is held back: the resolver code of the gate it fails, in check order.
HELD_TEXT = {
    "KB-RES-013": "needs another MCU family",
    "KB-RES-014": "needs a newer kb-core",
    "KB-RES-018": "requires an owner-task bus",
    "KB-RES-019": "does not support the bus kind in use",
    "KB-RES-024": "needs a newer C++ standard than the image's language block",
    "KB-RES-025": "needs a newer compiler than the image's language block",
}

WARNING_TEXT = {
    "KB-RES-W01": "release candidate",
    "KB-RES-W03": "kb-core development revision",
    "KB-RES-W04": "older driver version for the language block",
    "KB-UPG-W01": "breaking change",
    "KB-UPG-W02": "newer version held back",
    "KB-UPG-W03": "known vulnerabilities remain",
    "KB-UPG-W04": "lock commit differs from the catalogue",
}

KINDS = ("release", "rc", "dev")


def _is_obj(v):
    return isinstance(v, dict)


def _is_str(v):
    return isinstance(v, str)


def _key(entry):
    return (entry["code"], entry["subject"])


def _fail(c):
    return {"ok": False, "errors": sorted(c.errors, key=_key), "warnings": sorted(c.warnings, key=_key),
            "rows": [], "image": None, "lock": None}


def _lock_driver_versions(lock, c):
    """{name: version} of a lock's drivers, or None (with KB-UPG-002) when it is not a schema-1 lock."""
    ok = (_is_obj(lock) and lock.get("schema") == 1 and not isinstance(lock.get("schema"), bool)
          and _is_obj(lock.get("kb_core")) and _is_str(lock["kb_core"].get("version"))
          and isinstance(lock.get("drivers"), list) and isinstance(lock.get("ports", []), list))
    if ok:
        for d in lock["drivers"]:
            if not (_is_obj(d) and _is_str(d.get("name")) and _is_str(d.get("version"))):
                ok = False
        for p in lock.get("ports", []):
            if not _is_obj(p) or ("version" in p and not _is_str(p["version"])):
                ok = False
    if not ok:
        c.error("KB-UPG-002", "lock", "the lock is not a kb lock (schema 1) with kb_core and driver versions")
        return None
    return {d["name"]: d["version"] for d in lock["drivers"]}


def _lock_facts(lock):
    """{component: (kind or None, commit or None)} of a lock, for comparing with the catalogue."""
    out = {}
    if not _is_obj(lock):
        return out

    def fact(entry):
        kind = entry.get("kind") if entry.get("kind") in KINDS else None
        commit = entry.get("commit") if _is_str(entry.get("commit")) else None
        return (kind, commit)

    if _is_obj(lock.get("kb_core")):
        out["kb-core"] = fact(lock["kb_core"])
    for port in lock.get("ports") or []:
        if _is_obj(port) and port.get("in_tree") is False and _is_str(port.get("name")):
            out[f"port-{port['name']}"] = fact(port)
    for d in lock.get("drivers") or []:
        if _is_obj(d) and _is_str(d.get("name")):
            out[d["name"]] = fact(d)
    return out


def _pin_current(image, lock, c):
    """The image with every version taken from the lock, so 'current' means what the project builds today."""
    if lock is None:
        return image
    locked = _lock_driver_versions(lock, c)
    if locked is None:
        return image
    shape_ok = (_is_obj(image) and _is_obj(image.get("kb_core")) and _is_obj(image.get("mcu"))
                and isinstance(image.get("drivers", []), list))
    if not shape_ok:
        return image  # the resolver reports the shape
    cur = copy.deepcopy(image)
    kb_ver = lock["kb_core"]["version"]
    if _is_str(cur["kb_core"].get("version")) and cur["kb_core"]["version"] != kb_ver:
        c.error("KB-UPG-002", "kb_core.version",
                f"the image names kb-core {cur['kb_core']['version']}, the lock {kb_ver}")
    cur["kb_core"]["version"] = kb_ver
    for port in lock.get("ports", []):
        if port.get("in_tree") is False and _is_str(port.get("version")):
            have = cur["mcu"].get("port")
            if _is_obj(have) and _is_str(have.get("version")) and have["version"] != port["version"]:
                c.error("KB-UPG-002", "mcu.port.version",
                        f"the image names port {have['version']}, the lock {port['version']}")
            cur["mcu"]["port"] = {"version": port["version"]}
    seen = set()
    drivers = []
    for item in cur.get("drivers", []):
        name = item if _is_str(item) else item.get("name") if _is_obj(item) else None
        if not _is_str(name):
            drivers.append(item)
            continue
        seen.add(name)
        if name not in locked:
            c.error("KB-UPG-002", name, f"driver '{name}' is in the image but not in the lock")
            drivers.append(item)
            continue
        if _is_obj(item) and _is_str(item.get("version")) and item["version"] != locked[name]:
            c.error("KB-UPG-002", name, f"the image names {name} {item['version']}, the lock {locked[name]}")
        drivers.append({"name": name, "version": locked[name]})
    for name in sorted(set(locked) - seen):
        c.error("KB-UPG-002", name, f"driver '{name}' is in the lock but not in the image")
    if "drivers" in cur:
        cur["drivers"] = drivers
    return cur


def _find(items, version):
    for v in items:
        if v.get("version") == version:
            return v
    return None


def _newest(versions, kind):
    best = None
    for v in versions:
        if v.get("kind") == kind and (best is None or semver.compare(v["version"], best["version"]) > 0):
            best = v
    return best


def _pick(versions, current, allow_rc, forced, gate, subject, c):
    """(chosen version entry, held [(version, code)]) or (None, []) after an error."""
    cur = _find(versions, current["version"])
    if forced is not None:
        entry = _find(versions, forced) if semver.parse_version(forced) is not None else None
        if entry is None:
            c.error("KB-UPG-004", subject, f"{subject} has no version {forced} in the catalogue")
            return None, []
        if semver.compare(entry["version"], current["version"]) < 0:
            c.error("KB-UPG-005", subject, f"{subject} {forced} is older than the current {current['version']}")
            return None, []
        g = gate(entry)
        if g is not None:
            c.error(g, subject, f"{subject} {forced}: {HELD_TEXT[g]}")
            return None, []
        return entry, []
    kinds = ("release", "rc") if allow_rc else ("release",)
    floor = 0 if current["kind"] == "dev" else 1  # a release of the same version replaces a dev revision
    cands = [v for v in versions
             if v.get("kind") in kinds and semver.compare(v["version"], current["version"]) >= floor]
    cands.sort(key=lambda v: semver.parse_version(v["version"]), reverse=True)
    held = []
    for v in cands:
        g = gate(v)
        if g is None:
            return v, held
        held.append({"version": v["version"], "code": g})
    return cur, held


def _breaking(a, b):
    if a == b:
        return False
    ka, kb = semver.parse_version(a), semver.parse_version(b)
    if ka[0] != kb[0]:
        return True
    if ka[0] == 0:
        if ka[1] != kb[1]:
            return True
        if ka[1] == 0 and ka[2] != kb[2]:
            return True
    return False


def _change(a, b):
    if a == b:
        return "prerelease"  # same X.Y.Z[-rc.N], only the kind changed (a dev revision replaced by its release)
    ka, kb = semver.parse_version(a), semver.parse_version(b)
    for i, name in enumerate(("major", "minor", "patch")):
        if ka[i] != kb[i]:
            return name
    return "prerelease"


def _osv_ids(entry):
    osv = entry.get("osv") if entry else None
    if not _is_obj(osv):
        return None
    findings = osv.get("findings")
    if not isinstance(findings, list):
        findings = []
    return sorted({f["id"] for f in findings if _is_obj(f) and _is_str(f.get("id"))})


def _row(name, kind, repo, versions, cur, chosen, held, relocked=False):
    nr = _newest(versions, "release")
    nrc = _newest(versions, "rc")
    if nrc is not None and nr is not None and semver.compare(nrc["version"], nr["version"]) < 0:
        nrc = None
    osv_from = _osv_ids(_find(versions, cur["version"]))
    osv_to = _osv_ids(chosen)
    fixed = sorted(set(osv_from or []) - set(osv_to or [])) if osv_to is not None else []
    changed = chosen["version"] != cur["version"] or chosen["kind"] != cur["kind"] or relocked
    notes = None
    if changed and chosen.get("tag"):
        notes = f"https://github.com/{repo}/releases/tag/{chosen['tag']}"
    return {
        "name": name,
        "kind": kind,
        "from": cur["version"],
        "from_kind": cur["kind"],
        "to": chosen["version"],
        "to_kind": chosen["kind"],
        "change": ("none" if not changed else _change(cur["version"], chosen["version"])
                   if chosen["version"] != cur["version"] else "prerelease" if chosen["kind"] != cur["kind"]
                   else "commit"),
        "lock_differs": relocked,
        "breaking": _breaking(cur["version"], chosen["version"]),
        "newest_release": nr["version"] if nr else None,
        "newest_rc": nrc["version"] if nrc else None,
        "held": held,
        "osv_from": osv_from,
        "osv_to": osv_to,
        "fixed": fixed,
        "notes": notes,
    }


def plan(catalog, image, lock=None, to=None, allow_rc=False):
    """Plan an upgrade of ``image`` (optionally with its current ``lock``) to newer catalogued versions.

    ``to`` maps a component (``kb-core``, ``port-<family>`` or a driver name) to a version to move it to;
    ``allow_rc`` lets release candidates be picked automatically. Returns ``{ok, errors, warnings, rows, image,
    lock}``: one row per component (kb-core, an extracted port, each driver), the new image (every version
    explicit) and its lock. Errors and warnings are sorted by code then subject.
    """
    c = resolver._Collector()
    to = dict(to) if _is_obj(to) else {}
    current = _pin_current(image, lock, c)
    if c.errors:
        return _fail(c)
    res = resolver.resolve(catalog, current)
    if not res["ok"]:
        c.error("KB-UPG-001", "image", "the current image does not resolve; fix it with kb resolve first")
        c.errors.extend(res["errors"])
        return _fail(c)
    cur = res["lock"]
    facts = _lock_facts(lock)

    def current_of(name, entry):
        """The current {version, kind} and whether the lock's commit differs from the catalogue's (KB-UPG-W04)."""
        kind, commit = facts.get(name, (None, None))
        differs = (commit is not None and commit != entry["commit"]) or (kind is not None and kind != entry["kind"])
        if differs:
            c.warn("KB-UPG-W04", name, f"the lock pins {name} {entry['version']} at {(commit or '?')[:12]} "
                   f"({kind or entry['kind']}), the catalogue at {entry['commit'][:12]} ({entry['kind']})")
        return {"version": entry["version"], "kind": kind if differs and kind else entry["kind"]}, differs

    fam = cur["mcu"]["family"]
    mcu = next(m for m in catalog["mcus"] if m["family"] == fam)
    port_lock = cur["ports"][0]
    port_name = f"port-{port_lock['name']}"
    drivers = {d["name"]: d for d in catalog["drivers"]}
    known = {"kb-core"} | {d["name"] for d in cur["drivers"]}
    if port_lock.get("in_tree") is False:
        known.add(port_name)
    for key in sorted(to):
        if key not in known:
            why = (f"{key} is inside kb-core and moves with it" if key == port_name
                   else f"{key} is not in the image")
            c.error("KB-UPG-003", key, why)
    if c.errors:
        return _fail(c)

    # kb-core first: the drivers' kb_core_min gates are checked against its target.
    kb_versions = catalog["kb_core"]["versions"]
    kb_cur, kb_relock = current_of("kb-core", cur["kb_core"])
    want = resolver.wanted_language(current)

    def lang_gate(v):
        bad = None if want is None else resolver.misfit(v, want)
        return None if bad is None else bad[0]

    kb_new, kb_held = _pick(kb_versions, kb_cur, allow_rc, to.get("kb-core"), lang_gate, "kb-core", c)
    rows = []
    if kb_new is not None:
        rows.append(_row("kb-core", "kb-core", catalog["kb_core"]["repo"], kb_versions, kb_cur, kb_new, kb_held,
                         kb_relock))

    port_new = None
    if port_lock.get("in_tree") is False:
        pv = mcu["port"]["versions"]
        p_cur, p_relock = current_of(port_name, port_lock)
        port_new, p_held = _pick(pv, p_cur, allow_rc, to.get(port_name), lambda v: None, port_name, c)
        if port_new is not None:
            rows.append(_row(port_name, "port", port_lock["repo"], pv, p_cur, port_new, p_held, p_relock))

    buses_of = {}
    for bus in cur["buses"]:
        for dev in bus["devices"]:
            buses_of.setdefault(dev, []).append(bus)
    chosen = {}
    for d in cur["drivers"]:
        name = d["name"]
        entry = drivers[name]

        def gate(v, name=name):
            if "any" not in v["mcus"] and fam not in v["mcus"]:
                return "KB-RES-013"
            kmin = v.get("kb_core_min")
            if kb_new is not None and kmin is not None and semver.compare(kmin, kb_new["version"]) > 0:
                return "KB-RES-014"
            for bus in buses_of.get(name, []):
                if v.get("requires_owner") and bus["model"] == "guarded":
                    return "KB-RES-018"
            for bus in buses_of.get(name, []):
                if bus["kind"] is not None and bus["kind"] not in v["bus"]:
                    return "KB-RES-019"
            return lang_gate(v)

        d_cur, d_relock = current_of(name, d)
        new, held = _pick(entry["versions"], d_cur, allow_rc, to.get(name), gate, name, c)
        if new is not None:
            chosen[name] = new["version"]
            rows.append(_row(name, "driver", d["repo"], entry["versions"], d_cur, new, held, d_relock))
    if c.errors:
        return _fail(c)

    new_image = copy.deepcopy(current)
    new_image["kb_core"] = {"version": kb_new["version"]}
    if port_new is not None:
        new_image["mcu"]["port"] = {"version": port_new["version"]}
    if "drivers" in new_image:
        new_image["drivers"] = [{"name": n, "version": chosen[n]} for n in sorted(chosen)]
    after = resolver.resolve(catalog, new_image)
    if not after["ok"]:  # every gate above mirrors a resolver rule, so this means the two drifted apart
        c.errors.extend(after["errors"])
        return _fail(c)
    c.warnings.extend(after["warnings"])
    for r in rows:
        if r["breaking"]:
            c.warn("KB-UPG-W01", r["name"], f"{r['name']} {r['from']} to {r['to']} is a breaking change")
        if r["held"]:
            c.warn("KB-UPG-W02", r["name"], f"{r['name']} {r['held'][0]['version']} is held back: "
                   f"{HELD_TEXT.get(r['held'][0]['code'], r['held'][0]['code'])}")
        if r["osv_to"]:
            c.warn("KB-UPG-W03", r["name"],
                   f"{r['name']} {r['to']} has known vulnerabilities: {', '.join(r['osv_to'])}")
    return {"ok": True, "errors": [], "warnings": sorted(c.warnings, key=_key), "rows": rows, "image": new_image,
            "lock": after["lock"]}


# --- emitted files ------------------------------------------------------------------------------------------


def _q(text):
    return json.dumps(text, ensure_ascii=False)


def _flow(pairs):
    return "{" + ", ".join(f"{k}: {v}" for k, v in pairs) + "}"


def image_text(image):
    """A kb-image.yml in one fixed style: block keys, flow values, every string double-quoted."""
    lines = [
        "# kb-image.yml: the selection of one firmware build (schema 1, docs/design/assembler.md).",
        "# Written by an upgrade (kb upgrade or the configurator); resolve it with: kb resolve kb-image.yml",
        f"schema: {image['schema']}",
    ]
    if "image" in image:
        lines.append(f"image: {_q(image['image'])}")
    kb = image["kb_core"]
    lines.append(f"kb_core: {_flow([('version', _q(kb['version']))]) if 'version' in kb else '{}'}")
    mcu = image["mcu"]
    pairs = [("family", _q(mcu["family"]))]
    if "target" in mcu:
        pairs.append(("target", _q(mcu["target"])))
    if "port" in mcu:
        port = mcu["port"]
        pairs.append(("port", _flow([("version", _q(port["version"]))]) if "version" in port else "{}"))
    lines.append(f"mcu: {_flow(pairs)}")
    if "rtos" in image:
        lines.append(f"rtos: {_q(image['rtos'])}")
    if "language" in image:
        lang = image["language"]
        pairs = [("standard", str(int(lang["standard"])))]
        if "compilers" in lang:
            pairs.append(("compilers", _flow([(k, _q(lang["compilers"][k])) for k in sorted(lang["compilers"])])))
        lines.append(f"language: {_flow(pairs)}")
    if "drivers" in image:
        if not image["drivers"]:
            lines.append("drivers: []")
        else:
            lines.append("drivers:")
            for d in image["drivers"]:
                if _is_str(d):
                    d = {"name": d}
                pairs = [("name", _q(d["name"]))]
                if "version" in d:
                    pairs.append(("version", _q(d["version"])))
                lines.append(f"  - {_flow(pairs)}")
    if "buses" in image:
        if not image["buses"]:
            lines.append("buses: {}")
        else:
            lines.append("buses:")
            for key in sorted(image["buses"]):
                bus = image["buses"][key]
                pairs = [("model", _q(bus["model"]))]
                if "kind" in bus:
                    pairs.append(("kind", _q(bus["kind"])))
                pairs.append(("devices", "[" + ", ".join(_q(x) for x in bus["devices"]) + "]"))
                lines.append(f"  {key}: {_flow(pairs)}")
    return "\n".join(lines) + "\n"


def _cell(v):
    return "-" if v is None else str(v)


def _vulns(ids):
    if ids is None:
        return "not scanned"
    return ", ".join(ids) if ids else "none"


def report_text(result, allow_rc=False):
    """The upgrade report (Markdown) of a successful plan."""
    lock = result["lock"]
    rows = result["rows"]
    kb_row = rows[0]
    changed = [r for r in rows if r["change"] != "none"]
    lines = [
        f"# Upgrade report: {lock['image']}",
        "",
        f"kb-core {kb_row['from']} to {kb_row['to']}, MCU {lock['mcu']['family']}/{lock['mcu']['target']}, "
        f"RTOS {lock['rtos']}. Release candidates {'allowed' if allow_rc else 'not picked unless named'}.",
        "",
        (f"{len(changed)} of {len(rows)} component(s) change." if changed
         else "Every component is at its newest selectable version."),
        "",
        "| Component | From | To | Change | Breaking | Newest release | Newer rc | Known vulnerabilities |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in rows:
        vul = _vulns(r["osv_from"]) if r["change"] == "none" else f"{_vulns(r['osv_from'])} -> {_vulns(r['osv_to'])}"
        lines.append(f"| {r['name']} | {r['from']} | {r['to']} | {r['change']} | {'yes' if r['breaking'] else 'no'} | "
                     f"{_cell(r['newest_release'])} | {_cell(r['newest_rc'])} | {vul} |")
    lines += ["", "## Breaking changes", ""]
    brk = [r for r in rows if r["breaking"]]
    for r in brk:
        lines.append(f"- {r['name']} {r['from']} -> {r['to']}: read its release notes and CHANGELOG before building: "
                     f"{r['notes'] or 'no release page'}")
    if not brk:
        lines.append("None by version number. Read each changed component's release notes all the same.")
    lines += ["", "## Held back", ""]
    held = [(r, h) for r in rows for h in r["held"]]
    for r, h in held:
        lines.append(f"- {r['name']} {h['version']}: {HELD_TEXT.get(h['code'], h['code'])} ({h['code']})")
    if not held:
        lines.append("None.")
    lines += ["", "## Known vulnerabilities", ""]
    fixed = [(r, i) for r in rows for i in r["fixed"]]
    left = [(r, i) for r in rows for i in (r["osv_to"] or [])]
    unscanned = [r["name"] for r in rows if r["osv_to"] is None]
    for r, i in fixed:
        lines.append(f"- fixed by this upgrade: {i} ({r['name']})")
    for r, i in left:
        lines.append(f"- still present: {i} ({r['name']} {r['to']})")
    if not fixed and not left:
        lines.append("- none known in the scanned components")
    if unscanned:
        lines.append(f"- not scanned: {', '.join(unscanned)}")
    lines += ["", "## Warnings", ""]
    for w in result["warnings"]:
        lines.append(f"- {w['code']} {w['subject']}: {WARNING_TEXT.get(w['code'], w['code'])}")
    if not result["warnings"]:
        lines.append("None.")
    lines += [
        "",
        "## Next steps",
        "",
        "1. Review the changes to `kb-image.yml` and `kb-image.lock.yml` with `git diff`.",
        "2. Run `sh kb-fetch.sh` to move every submodule to its new tag, then build and run your tests.",
        "3. Check the compatibility of the new set: a tested version set, or an untested combination.",
    ]
    return "\n".join(lines) + "\n"


def files(result, allow_rc=False):
    """Every file an upgrade writes: the new image, its lock and build files, and the report."""
    out = emit.files(result["lock"])
    out[IMAGE_FILE] = image_text(result["image"])
    out[REPORT_FILE] = report_text(result, allow_rc)
    return out
