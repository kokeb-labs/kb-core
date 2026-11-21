# `kb catalog build`: the catalogue (catalog/schemas/catalog.schema.json) from each driver's driver.yml at each of its
# release and release-candidate tags, read offline from local clones (git) or online (gh api), plus kb-core's own
# versions and the platform table. Deterministic: sorted, no timestamps.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Build the kb catalogue."""

import base64
import json
import os
import re
import subprocess

from . import platforms, repolayout, semver, yamlio

BUSES = ("adc", "can", "canopen", "gpio", "i2c", "modbus", "onewire", "pio", "spi", "uart")
NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")


class CatalogError(Exception):
    """The catalogue cannot be built; the message names the source."""


def detect_license(text):
    """SPDX id of a LICENSE file: MIT when it is the MIT text, else NOASSERTION."""
    if text is None:
        return "NOASSERTION"
    head = " ".join(text.split()[:40])
    if head.startswith("MIT License") and "Permission is hereby granted, free of charge" in text:
        return "MIT"
    return "NOASSERTION"


def version_entry(doc, tag, commit, license_text, where):
    """One catalogue version from driver.yml at a tag. Returns (entry, problems)."""
    problems = []
    if not isinstance(doc, dict):
        return None, [f"{where}: driver.yml at {tag} is not a mapping"]
    version = tag[1:]
    # An rc tag carries the version it is a candidate for: v0.1.0-rc.1 has driver.yml version 0.1.0.
    release = version.split("-", 1)[0]
    if doc.get("version") != release:
        problems.append(f"{where}: driver.yml version {doc.get('version')!r} at {tag} is not {release} (PIN-006)")
    req = doc.get("requires_owner", False)
    if not isinstance(req, bool):
        problems.append(f"{where}: requires_owner at {tag} must be true or false")
    bindings = doc.get("bindings", [])
    if not isinstance(bindings, list) or any(not isinstance(b, str) for b in bindings):
        problems.append(f"{where}: bindings at {tag} must be a list of binding names")
    bus = doc.get("bus")
    if not isinstance(bus, list) or not bus or any(b not in BUSES for b in bus):
        problems.append(f"{where}: bus at {tag} must be a list of {', '.join(BUSES)}")
    mcus = doc.get("mcus")
    if not isinstance(mcus, list) or not mcus or any(not isinstance(m, str) for m in mcus):
        problems.append(f"{where}: mcus at {tag} must be a list such as [any] or [esp32]")
    kmin = doc.get("kb_core_min")
    if kmin is not None and semver.parse_version(str(kmin)) is None:
        problems.append(f"{where}: kb_core_min {kmin!r} at {tag} is not X.Y.Z")
    authors = doc.get("authors", [])
    if not isinstance(authors, list) or any(not isinstance(a, str) for a in authors):
        problems.append(f"{where}: authors at {tag} must be a list of strings")
    if problems:
        return None, problems
    lic = doc.get("license") or detect_license(license_text)
    return {
        "version": version, "tag": tag, "commit": commit, "kind": semver.tag_kind(tag),
        "bus": sorted(bus), "mcus": sorted(m.lower() for m in mcus), "peripheral": doc.get("peripheral"),
        "kb_core_min": None if kmin is None else str(kmin), "requires_owner": bool(doc.get("requires_owner", False)),
        "license": lic, "authors": list(authors), **({"bindings": list(bindings)} if bindings else {}),
    }, []


def apply_binding_facts(catalogue, facts):
    """Interim source of binding facts until a kb-core release carries them: the configurator's platform file
    (configurator/data/platforms.json, "bindings": {name: {requires_owner}}). A fact only ever adds
    requires_owner; driver.yml can set it too."""
    for drv in catalogue["drivers"]:
        fact = facts.get(drv["name"]) or {}
        if fact.get("requires_owner"):
            for v in drv["versions"]:
                v["requires_owner"] = True
    return catalogue


def build(sources, kb_core_versions, port_versions=None):
    """The catalogue from raw sources.

    ``sources``: list of {name, repo, folder, pinned, tags: [{tag, commit, driver_yml, license}]} where driver_yml
    is the parsed document at that tag (None when missing). Returns (catalogue, problems).
    """
    problems = []
    drivers = []
    names = set()
    for src in sorted(sources, key=lambda s: s["name"]):
        where = src["repo"]
        if not NAME_RE.match(src["name"]):
            problems.append(f"{where}: driver name {src['name']!r} is not [a-z][a-z0-9_]*")
            continue
        if src["name"] in names:
            problems.append(f"{where}: driver {src['name']} appears twice")
            continue
        names.add(src["name"])
        versions = []
        for t in src["tags"]:
            if not semver.is_tag(t["tag"]):
                continue
            if t.get("driver_yml") is None:
                problems.append(f"{where}: no driver.yml at {t['tag']}")
                continue
            if isinstance(t["driver_yml"], dict) and t["driver_yml"].get("name") != src["name"]:
                problems.append(f"{where}: driver.yml name {t['driver_yml'].get('name')!r} at {t['tag']} is not "
                                f"{src['name']} (PIN-006)")
                continue
            entry, errs = version_entry(t["driver_yml"], t["tag"], t["commit"], t.get("license"), where)
            problems += errs
            if entry is not None:
                versions.append(entry)
        versions.sort(key=lambda v: semver.parse_version(v["version"]), reverse=True)
        drivers.append({"name": src["name"], "repo": src["repo"], "folder": src["folder"],
                        "pinned": src.get("pinned"), "versions": versions})
    catalogue = {
        "schema": 1,
        "kb_core": {"repo": platforms.KB_CORE_REPO, "license": platforms.KB_CORE_LICENSE,
                    "authors": list(platforms.KB_CORE_AUTHORS),
                    "versions": sorted(kb_core_versions, key=lambda v: semver.parse_version(v["version"]),
                                       reverse=True)},
        "parts": list(platforms.PARTS),
        "rtos": [dict(r) for r in platforms.RTOS],
        "mcus": platforms.mcus(port_versions),
        "drivers": drivers,
    }
    return catalogue, problems


# --- offline: local clones --------------------------------------------------------------------------------------

def _git(repo_dir, *args):
    res = subprocess.run(["git", "-C", repo_dir, *args], capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return None
    return res.stdout


def repo_slug(repo_dir, org):
    """org/name of a clone from its origin URL, else org/<folder name>."""
    url = (_git(repo_dir, "remote", "get-url", "origin") or "").strip()
    m = re.search(r"github\.com[:/]+([^/]+)/([^/]+?)(\.git)?/?$", url)
    if m:
        return f"{m.group(1)}/{m.group(2)}"
    return f"{org}/{os.path.basename(os.path.abspath(repo_dir))}"


def local_tags(repo_dir):
    out = _git(repo_dir, "tag", "--list", "v*") or ""
    tags = []
    for tag in sorted(t for t in out.split() if semver.is_tag(t)):
        commit = (_git(repo_dir, "rev-list", "-n", "1", tag) or "").strip()
        tags.append((tag, commit))
    return tags


def local_source(repo_dir, org, pinned=None):
    """The raw source of one driver clone."""
    if not os.path.isfile(os.path.join(repo_dir, "driver.yml")):
        raise CatalogError(f"{repo_dir}: no driver.yml (not a driver checkout)")
    head = yamlio.load(os.path.join(repo_dir, "driver.yml"))
    name = head.get("name") if isinstance(head, dict) else None
    if not isinstance(name, str):
        raise CatalogError(f"{repo_dir}: driver.yml has no name")
    tags = []
    for tag, commit in local_tags(repo_dir):
        text = _git(repo_dir, "show", f"{tag}:driver.yml")
        doc = yamlio.loads(text, f"{repo_dir}@{tag}:driver.yml") if text is not None else None
        tags.append({"tag": tag, "commit": commit, "driver_yml": doc, "license": _git(repo_dir, "show", f"{tag}:LICENSE")})
    slug = repo_slug(repo_dir, org)
    return {"name": name, "repo": slug, "folder": slug.split("/")[-1], "pinned": (pinned or {}).get(name),
            "tags": tags}


def driver_dirs(paths):
    """Each path is a driver clone, or a folder whose driver-* subfolders are clones."""
    found = []
    for p in paths:
        if os.path.isfile(os.path.join(p, "driver.yml")):
            found.append(p)
            continue
        if not os.path.isdir(p):
            raise CatalogError(f"{p}: not a directory")
        for d in sorted(os.listdir(p)):
            sub = os.path.join(p, d)
            if d.startswith("driver-") and os.path.isfile(os.path.join(sub, "driver.yml")):
                found.append(sub)
    return found


def kb_core_versions(kb_dir, include_head):
    """kb-core's own release and rc tags, plus the working revision as kind dev when asked."""
    versions = [{"version": tag[1:], "tag": tag, "commit": commit, "kind": semver.tag_kind(tag)}
                for tag, commit in local_tags(kb_dir)]
    if include_head:
        text = open(os.path.join(kb_dir, "cmake", "kb_version.cmake"), encoding="utf-8").read()
        parts = [re.search(rf"set\(KB_VERSION_{k} (\d+)\)", text) for k in ("MAJOR", "MINOR", "PATCH")]
        if not all(parts):
            raise CatalogError(f"{kb_dir}: cmake/kb_version.cmake does not set the version")
        version = ".".join(m.group(1) for m in parts)
        commit = (_git(kb_dir, "rev-parse", "HEAD") or "").strip()
        if not any(v["version"] == version for v in versions):
            versions.append({"version": version, "tag": None, "commit": commit, "kind": "dev"})
    return versions


def pins(kb_dir):
    """name -> pinned version from the drivers.lock.yml in the drivers folder of the kb-core checkout."""
    path = os.path.join(kb_dir, repolayout.path("drivers", kb_dir), "drivers.lock.yml")
    if not os.path.isfile(path):
        return {}
    doc = yamlio.load(path) or {}
    return {d["name"]: d["tag"][1:] for d in doc.get("drivers") or [] if semver.is_tag(d.get("tag"))}


# --- online: gh api ---------------------------------------------------------------------------------------------

def _gh(*args):
    res = subprocess.run(["gh", "api", *args], capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return None
    return res.stdout


def _gh_file(repo, path, ref):
    out = _gh(f"repos/{repo}/contents/{path}?ref={ref}")
    if out is None:
        return None
    return base64.b64decode(json.loads(out)["content"]).decode("utf-8")


def online_repos(org):
    out = _gh("--paginate", f"orgs/{org}/repos?per_page=100", "--jq", ".[].name")
    if out is None:
        raise CatalogError(f"gh api: cannot list the repositories of {org} (is gh logged in?)")
    return sorted(n for n in out.split() if n.startswith("driver-"))


def online_source(org, repo_name, pinned=None):
    repo = f"{org}/{repo_name}"
    out = _gh("--paginate", f"repos/{repo}/tags?per_page=100", "--jq", '.[] | "\\(.name) \\(.commit.sha)"')
    if out is None:
        raise CatalogError(f"gh api: cannot list the tags of {repo}")
    tags = []
    name = None
    for line in sorted(out.splitlines()):
        tag, commit = line.split()
        if not semver.is_tag(tag):
            continue
        text = _gh_file(repo, "driver.yml", tag)
        doc = yamlio.loads(text, f"{repo}@{tag}:driver.yml") if text is not None else None
        if isinstance(doc, dict) and isinstance(doc.get("name"), str):
            name = doc["name"]
        tags.append({"tag": tag, "commit": commit, "driver_yml": doc, "license": _gh_file(repo, "LICENSE", tag)})
    if name is None:
        name = repo_name[len("driver-"):].replace("-", "_")
    return {"name": name, "repo": repo, "folder": repo_name, "pinned": (pinned or {}).get(name), "tags": tags}


# --- the configurator's site catalogue --------------------------------------------------------------------------

_HOLDER_RE = re.compile(r"^Copyright \(c\) (?:\d{4}(?:-\d{4})? )?(.+)$")


def _holders(copyright_line):
    m = _HOLDER_RE.match(copyright_line or "")
    return [m.group(1)] if m else []


def _osv(entry):
    # JavaScript truthiness, as in shared.js osvOf: an object (even empty) is kept, null, false, 0 and "" are not.
    o = entry.get("osv")
    return None if o is None or o is False or o == "" or (isinstance(o, (int, float)) and o == 0) else o


def from_site(site):
    """kb's catalogue derived from the configurator's site catalogue (configurator/data/catalog.json).

    The same derivation as KB.shared.kbCatalog in configurator/js/shared.js, field for field, so `kb outdated` and
    `kb upgrade` given the site's catalogue plan exactly what the site plans (configurator/tests check both agree).
    """
    def releases(items):
        return [{"version": r["version"], "tag": r["tag"], "commit": r["commit"], "kind": r["kind"], "osv": _osv(r)}
                for r in items]

    def version(d, v):
        req = v["requires_owner"] if isinstance(v.get("requires_owner"), bool) else d["requires_owner"]
        return {"version": v["version"], "tag": v["tag"], "commit": v["commit"], "kind": v["kind"], "bus": list(v["bus"]),
                "mcus": list(v["mcus"]), "peripheral": v["peripheral"], "kb_core_min": v["kb_core_min"],
                "requires_owner": req, "license": d["licence"]["spdx"], "authors": _holders(d["licence"]["copyright"]),
                "osv": _osv(v)}

    return {
        "schema": 1,
        "kb_core": {"repo": site["kb_core"]["repo"], "license": "MIT", "authors": ["The kb-core authors"],
                    "versions": releases(site["kb_core"]["releases"])},
        "parts": list(site["kb_core"]["parts"]),
        "rtos": [{"name": r["id"], "status": r["status"]} for r in site["rtos"]],
        "mcus": [{
            "family": f["id"], "status": f["status"], "targets": [t["id"] for t in f["targets"]], "rtos": list(f["rtos"]),
            "default_rtos": f["default_rtos"],
            "port": ({"name": f["port"]["name"], "repo": f["port"]["repo"], "path": f["port"]["path"],
                      "versions": releases(f["port"]["releases"])} if f.get("port") else None),
        } for f in site["families"]],
        "drivers": [{"name": d["name"], "repo": d["repo"], "folder": d["folder"], "pinned": d["pinned"],
                     "versions": [version(d, v) for v in d["versions"]]} for d in site["drivers"]],
    }


def _is_list_of_dicts(v):
    return isinstance(v, list) and all(isinstance(x, dict) for x in v)


def load_any(doc, name):
    """A kb catalogue from ``doc``: kb's own (schema 1) as is, the configurator's site catalogue converted.

    Anything else raises yamlio.InputError, so the commands exit 2 with a message instead of a traceback.
    """
    if isinstance(doc, dict) and isinstance(doc.get("families"), list) and isinstance(doc.get("kb_core"), dict) \
            and "releases" in doc["kb_core"]:
        try:
            return from_site(doc)
        except (KeyError, TypeError, AttributeError) as exc:
            raise yamlio.InputError(f"{name}: a configurator catalogue with a missing or malformed field ({exc})") \
                from None
    ok = (isinstance(doc, dict) and doc.get("schema") == 1 and isinstance(doc.get("kb_core"), dict)
          and _is_list_of_dicts(doc["kb_core"].get("versions")) and _is_list_of_dicts(doc.get("mcus"))
          and _is_list_of_dicts(doc.get("drivers")) and isinstance(doc.get("parts"), list)
          and _is_list_of_dicts(doc.get("rtos"))
          and all(_is_list_of_dicts(d.get("versions")) for d in doc["drivers"]))
    if not ok:
        raise yamlio.InputError(f"{name}: not a kb catalogue (schema 1: kb_core.versions, parts, rtos, mcus, drivers) "
                                "nor the configurator's data/catalog.json")
    return doc
