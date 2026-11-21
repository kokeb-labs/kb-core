#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Build configurator/data/catalog.json (and catalog.js, the same bytes wrapped for file:// use).

Inputs:
  * configurator/data/platforms.json: MCU families, targets, boards, RTOS backends, bus kinds, binding facts;
  * the local driver clones (<drivers-root>/driver-<chip>): tags and their commits, and at each tag the
    driver.yml, README.md (summary, "At a glance" rows, I2C addresses), LICENSE, docs/hardware.yml (control
    pins) and the chip figures in docs/diagrams/;
  * configurator/data/tags.json: which GitHub topics of a driver repository become search tags, and the category;
  * with --online and the gh tool signed in: the tags of every public repository (drivers, ports, kb-core), the
    files of a tag the local clone does not have, and the topics of every driver repository; a private repository
    contributes no online tags. Without --online the topics are not fetched (nothing is reused from an earlier
    build): the build says so, and each driver's tags come from the driver.yml fallback only.

Every value is read at the commit of a tag, never from a working tree, so the catalogue describes releases only
(ADR-0012). A tag whose commit differs between the clone and GitHub is an error (a moved tag). Output is
deterministic: sorted, no wall-clock time; source_date is the newest commit date of any component.

Usage:
  build_catalog.py [--drivers-root DIR] [--online] [--include-head] [--check] [--no-figures]

--include-head adds kb-core's main line (origin/main, else HEAD) as a development revision (kind dev, no tag), so a selection can be
resolved before kb-core's first release; the resolver warns about it (KB-RES-W03).
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONF = HERE.parent
sys.path.insert(0, str(HERE))
from jsonschema_lite import validate  # noqa: E402

sys.path.insert(0, str(CONF.parent / "tools"))
from kb import language, repolayout  # noqa: E402
import tags as kb_tags  # noqa: E402

sys.path.insert(0, str(CONF.parent / "tools" / "examples"))
import examples as kb_examples  # noqa: E402

TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?$")
DRIVER_ORG = "kokeb-labs-drivers"
FIGURE_KINDS = ("pinout", "outline", "blocks")
CAPABILITY_SKIP = {"Driver", "Runs on", "Status", "Safety"}


class BuildError(Exception):
    pass


# -- small helpers -------------------------------------------------------------------------------------------------

def run(cmd: list[str], cwd: Path | None = None, check: bool = True) -> str | None:
    try:
        out = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=check)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout


def tag_key(tag: str) -> tuple:
    m = TAG_RE.match(tag)
    assert m, tag
    major, minor, patch, rc = m.groups()
    # A release sorts above every rc of the same version.
    return (int(major), int(minor), int(patch), 1 if rc is None else 0, int(rc or 0))


def utc(iso: str) -> str:
    return dt.datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone(dt.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def parse_flat_yaml(text: str) -> dict:
    """The flat key: value subset driver.yml uses (scalars, null, [a, b] lists, quoted strings)."""
    out: dict = {}
    for raw in text.splitlines():
        if raw.startswith(("#", "---", " ", "\t")) or ":" not in raw:
            continue
        key, _, value = raw.partition(":")
        value = re.sub(r"\s+#.*$", "", value).strip()
        out[key.strip()] = _scalar(value)
    return out


def _scalar(value: str):
    if value in ("", "null", "~"):
        return None
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        return [_scalar(v.strip()) for v in inner.split(",")] if inner else []
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    if value in ("true", "false"):
        return value == "true"
    return value


def strip_md(text: str) -> str:
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = text.replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", text).strip()


# -- reading a repository at a tag ---------------------------------------------------------------------------------

class Source:
    """Files of one repository at one commit, from the local clone or from GitHub."""

    def __init__(self, clone: Path | None, repo: str, commit: str, tag: str, online: bool):
        self.clone, self.repo, self.commit, self.tag, self.online = clone, repo, commit, tag, online
        self.local = clone is not None and run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=clone) is not None
        if not self.local and not online:
            raise BuildError(f"{repo}: commit {commit[:12]} of {tag} is not in the clone; run with --online")

    @property
    def origin(self) -> str:
        return "tag" if self.local else "tag-online"

    def read(self, path: str) -> bytes | None:
        if self.local:
            try:
                return subprocess.run(["git", "show", f"{self.commit}:{path}"], cwd=self.clone,
                                      capture_output=True, check=True).stdout
            except subprocess.CalledProcessError:
                return None
        out = run(["gh", "api", f"repos/{self.repo}/contents/{path}?ref={self.tag}", "--jq", ".content"])
        return base64.b64decode(out) if out else None

    def text(self, path: str) -> str | None:
        data = self.read(path)
        return data.decode("utf-8") if data is not None else None

    def listdir(self, path: str) -> list[str]:
        if self.local:
            out = run(["git", "ls-tree", "--name-only", f"{self.commit}:{path}"], cwd=self.clone) or ""
            return sorted(out.split())
        out = run(["gh", "api", f"repos/{self.repo}/contents/{path}?ref={self.tag}", "--jq", ".[].name"]) or ""
        return sorted(out.split())

    def date(self) -> str:
        if self.local:
            return utc(run(["git", "log", "-1", "--format=%cI", self.commit], cwd=self.clone).strip())
        out = run(["gh", "api", f"repos/{self.repo}/commits/{self.commit}", "--jq", ".commit.committer.date"])
        if not out:
            raise BuildError(f"{self.repo}: cannot read the date of {self.commit[:12]}")
        return utc(out.strip())


def local_tags(clone: Path | None) -> dict[str, str]:
    if clone is None or not (clone / ".git").exists():
        return {}
    out = run(["git", "for-each-ref", "--format=%(refname:short) %(objectname) %(*objectname)", "refs/tags"],
              cwd=clone) or ""
    tags = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and TAG_RE.match(parts[0]):
            tags[parts[0]] = parts[2] if len(parts) > 2 else parts[1]  # peeled commit of an annotated tag
    return tags


def online_tags(repo: str) -> dict[str, str] | None:
    # A private repository's tags are not releases anyone can fetch, and the published site must not depend on
    # what the signed-in account happens to see: they are left out (an empty set), the same as CI sees them.
    private = run(["gh", "api", f"repos/{repo}", "--jq", ".private"])
    if private is None:
        return None
    if private.strip() == "true":
        return {}
    out = run(["gh", "api", "--paginate", f"repos/{repo}/tags", "--jq", ".[] | .name + \" \" + .commit.sha"])
    if out is None:
        return None
    return {n: s for n, s in (line.split() for line in out.splitlines() if line.strip()) if TAG_RE.match(n)}


def online_topics(repo: str) -> list[str]:
    out = run(["gh", "api", f"repos/{repo}/topics", "--jq", ".names[]"])
    if out is None:
        raise BuildError(f"{repo}: cannot read the repository topics with gh; sign in or build without --online")
    return sorted(line.strip() for line in out.splitlines() if line.strip())


def merged_tags(repo: str, clone: Path | None, online: bool) -> dict[str, str]:
    tags = local_tags(clone)
    if online:
        remote = online_tags(repo)
        if remote is not None:
            for name, sha in remote.items():
                if name in tags and tags[name] != sha:
                    raise BuildError(f"{repo}: tag {name} is {sha[:12]} on GitHub but {tags[name][:12]} locally "
                                     "(a moved tag); fix it before building the catalogue")
                tags[name] = sha
    return tags


# -- one driver ----------------------------------------------------------------------------------------------------

def readme_facts(readme: str) -> tuple[str, list[dict], list[int] | None]:
    lines = readme.splitlines()
    summary_lines: list[str] = []
    started = False
    for line in lines[1:]:
        if not line.strip():
            if started:
                break
            continue
        if line.startswith(("[!", "<", "#", "|", "**")):
            if started:
                break
            continue
        started = True
        summary_lines.append(line.strip())
    summary = strip_md(" ".join(summary_lines))
    caps: list[dict] = []
    addresses: set[int] = set()
    known_addresses = False
    in_glance = False
    for line in lines:
        if line.startswith("## "):
            in_glance = line.strip() == "## At a glance"
            continue
        if line.startswith("<!--") and in_glance and caps:
            in_glance = False
        m = re.match(r"^\|\s*\*\*(.+?)\*\*\s*\|\s*(.+?)\s*\|\s*$", line)
        if not m or not in_glance:
            continue
        label, text = m.group(1).strip(), m.group(2)
        if label in ("Bus", "Transports") and "I2C" in text:
            for lo, hi in re.findall(r"`?0x([0-9A-Fa-f]{2})`?\s+to\s+`?0x([0-9A-Fa-f]{2})`?", text):
                addresses.update(range(int(lo, 16), int(hi, 16) + 1))
                known_addresses = True
            text_wo_ranges = re.sub(r"0x[0-9A-Fa-f]{2}`?\s+to\s+`?0x[0-9A-Fa-f]{2}", "", text)
            for single in re.findall(r"0x([0-9A-Fa-f]{2})", text_wo_ranges):
                addresses.add(int(single, 16))
                known_addresses = True
        if label not in CAPABILITY_SKIP:
            caps.append({"label": label, "text": strip_md(text)})
    return summary, caps, (sorted(addresses) if known_addresses else None)


def licence_facts(text: str | None) -> dict:
    if not text:
        return {"spdx": "NOASSERTION", "copyright": "NOASSERTION"}
    first = text.strip().splitlines()[0].strip()
    spdx = {"MIT License": "MIT", "Apache License": "Apache-2.0"}.get(first, "NOASSERTION")
    m = re.search(r"^Copyright \(c\) (.+)$", text, re.M)
    return {"spdx": spdx, "copyright": f"Copyright (c) {m.group(1).strip()}" if m else "NOASSERTION"}


def hardware_facts(text: str | None) -> tuple[str | None, str | None, int]:
    if not text:
        return None, None, 0
    chip = re.search(r"^chip:\s*(.+)$", text, re.M)
    package = re.search(r"^package:\s*\n(?:\s+.*\n)*?\s+type:\s*(.+)$", text, re.M)
    hooks = re.search(r"^hooks:\s*\n((?:[ \t-].*\n|\n)*)", text, re.M)
    kinds = re.findall(r"kind:\s*([a-z0-9-]+)", hooks.group(1)) if hooks else []
    optional = sum(1 for k in kinds if k.startswith("gpio"))
    return (_scalar(chip.group(1).strip()) if chip else None, _scalar(re.sub(r"\s+#.*$", "", package.group(1)).strip()) if package else None, optional)


def svg_alt(svg: str) -> str:
    title = re.search(r"<title[^>]*>(.*?)</title>", svg, re.S)
    desc = re.search(r"<desc[^>]*>(.*?)</desc>", svg, re.S)
    parts = [re.sub(r"\s+", " ", p.group(1)).strip() for p in (desc or title,) if p]
    return parts[0] if parts else "Chip figure"


def driver_requires(versions: list[dict], meta: dict, binding: dict) -> dict:
    """What the newest version needs from a build, as data the page filters on (availability() in resolver.mjs)."""
    if versions:
        v = versions[0]
        lang = {k: v["language"][k] for k in ("standard", "compilers") if k in v["language"]}
        return {"bus": list(v["bus"]), "mcus": list(v["mcus"]), "peripheral": v["peripheral"],
                "rtos": list(binding.get("rtos", [])), "kb_core_min": v["kb_core_min"], "language": lang}
    return {"bus": list(meta.get("bus") or []), "mcus": list(meta.get("mcus") or ["any"]),
            "peripheral": meta.get("peripheral"), "rtos": list(binding.get("rtos", [])),
            "kb_core_min": meta.get("kb_core_min"), "language": {"standard": meta.get("language_standard", 20)}}


def build_driver(clone: Path, online: bool, figures: bool, platforms: dict, rules: dict) -> tuple[dict, list[str]]:
    worktree = parse_flat_yaml((clone / "driver.yml").read_text())
    name = worktree["name"]
    repo = f"{DRIVER_ORG}/{clone.name}"
    tags = merged_tags(repo, clone, online)
    versions, dates = [], []
    latest_source = None
    binding = platforms.get("bindings", {}).get(name, {})
    for tag in sorted(tags, key=tag_key, reverse=True):
        src = Source(clone, repo, tags[tag], tag, online)
        meta_text = src.text("driver.yml")
        if meta_text is None:
            raise BuildError(f"{repo}@{tag}: no driver.yml")
        meta = parse_flat_yaml(meta_text)
        if meta.get("name") != name:
            raise BuildError(f"{repo}@{tag}: driver.yml name {meta.get('name')!r} differs from {name!r}")
        if f"v{meta.get('version')}" != re.sub(r"-rc\.\d+$", "", tag):
            raise BuildError(f"{repo}@{tag}: driver.yml version {meta.get('version')} does not match the tag")
        date = src.date()
        dates.append(date)
        lang, problems = language.normalise(language.block_from_text(meta_text), "driver.yml", f"{repo}@{tag}")
        if problems:
            raise BuildError("; ".join(problems))
        versions.append({
            "version": tag[1:],
            "tag": tag,
            "commit": tags[tag],
            "kind": "rc" if "-rc." in tag else "release",
            "date": date,
            "bus": list(meta.get("bus") or []),
            "mcus": list(meta.get("mcus") or ["any"]),
            "peripheral": meta.get("peripheral"),
            "kb_core_min": meta.get("kb_core_min"),
            # The rule of `kb catalog build` (tools/kb/catalog.py): driver.yml at the tag, or a platform binding fact.
            "requires_owner": meta.get("requires_owner") is True or bool(binding.get("requires_owner", False)),
            "language": lang,
            "metadata_source": src.origin,
            "osv": None,
        })
        latest_source = latest_source or src
    if any(v["kind"] == "release" for v in versions):
        status = "release"
    elif versions:
        status = "rc"
    else:
        status = "unpublished"

    # Card facts come from the newest tag; an unpublished driver shows only what its driver.yml names.
    readme = latest_source.text("README.md") if latest_source else None
    summary, caps, addresses = readme_facts(readme) if readme else ("", [], None)
    chip, package, optional = hardware_facts(latest_source.text("docs/hardware.yml") if latest_source else None)
    licence = licence_facts(latest_source.text("LICENSE") if latest_source else None)
    figs = []
    if latest_source:
        # The entries are always listed; the files are written only when figures is true (not with --check).
        listing = latest_source.listdir("docs/diagrams")
        out_dir = CONF / "assets" / "figures" / name
        for kind in FIGURE_KINDS:
            light, dark = f"chip-{kind}.svg", f"chip-{kind}.dark.svg"
            if light not in listing or dark not in listing:
                continue
            light_svg = latest_source.read(f"docs/diagrams/{light}")
            if figures:
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / light).write_bytes(light_svg)
                (out_dir / dark).write_bytes(latest_source.read(f"docs/diagrams/{dark}"))
            figs.append({
                "kind": kind,
                "light": f"assets/figures/{name}/{light}",
                "dark": f"assets/figures/{name}/{dark}",
                "alt": svg_alt(light_svg.decode("utf-8")),
            })
    topics = online_topics(repo) if online else None
    classified = kb_tags.driver_tags(topics, worktree, summary, list(versions[0]["bus"] if versions else worktree.get("bus") or []), rules)
    driver = {
        "name": name,
        "repo": repo,
        "url": f"https://github.com/{repo}.git",
        "folder": clone.name,
        "pinned": None,
        "chip": chip or name.upper(),
        "package": package,
        "summary": summary,
        "status": status,
        "licence": licence,
        "requires_owner": bool(binding.get("requires_owner", False)) or any(v["requires_owner"] for v in versions[:1]),
        "capabilities": caps,
        "figures": figs,
        "optional_pins": optional,
        "i2c_addresses": addresses if "i2c" in (versions[0]["bus"] if versions else worktree.get("bus") or []) else None,
        "tags": classified["tags"],
        "category": classified["category"],
        "requires": driver_requires(versions, worktree, binding),
        "versions": versions,
    }
    return driver, dates


def kb_core_head(clone: Path) -> dict:
    """kb-core's main line (origin/main when the clone has it, else HEAD) as a development revision."""
    ref = "origin/main" if run(["git", "rev-parse", "--verify", "--quiet", "origin/main"], cwd=clone) else "HEAD"
    commit = run(["git", "rev-parse", ref], cwd=clone).strip()
    text = run(["git", "show", f"{commit}:cmake/kb_version.cmake"], cwd=clone) or ""
    parts = [re.search(rf"set\(KB_VERSION_{k} (\d+)\)", text).group(1) for k in ("MAJOR", "MINOR", "PATCH")]
    date = utc(run(["git", "log", "-1", "--format=%cI", commit], cwd=clone).strip())
    return {"version": ".".join(parts), "tag": None, "commit": commit, "kind": "dev", "date": date, "bindings": [],
            "language": kb_language(run(["git", "show", f"{commit}:kb.yml"], cwd=clone), f"kb-core@{commit[:12]}"),
            "osv": None}


def kb_language(text: str | None, where: str) -> dict:
    """kb-core's language floor from its kb.yml at a revision (no file: the registry default, source default)."""
    lang, problems = language.normalise(language.block_from_text(text) if text is not None else None, "kb.yml", where)
    if problems:
        raise BuildError("; ".join(problems))
    return lang


def driver_pins(clone: Path | None) -> dict[str, str]:
    """name -> version from kb-core's the drivers.lock.yml of its drivers folder (flow entries {name: x, tag: vX.Y.Z, ...})."""
    if clone is None:
        return {}
    lock = clone / repolayout.path("drivers", str(clone)) / "drivers.lock.yml"
    if not lock.exists():
        return {}
    text = lock.read_text()
    pins = {}
    for block in re.split(r"^\s*-\s+", text, flags=re.M)[1:]:
        name = re.search(r"name:\s*([a-z0-9_]+)", block)
        tag = re.search(r"tag:\s*v([0-9][^\s,}]*)", block)
        if name and tag:
            pins[name.group(1)] = tag.group(1)
    return pins


BINDINGS_DIR = "components/lib/handlers/inc/kb/handlers/bindings"


def kb_core_bindings(src: Source) -> list[str]:
    """The drivers a kb-core tag binds: a header <name>.hpp in the handlers bindings folder at that tag."""
    return sorted(n[:-len(".hpp")] for n in src.listdir(BINDINGS_DIR) if n.endswith(".hpp"))


def build_releases(repo: str, clone: Path | None, online: bool, extra: dict | None = None,
                   bindings: bool = False) -> tuple[list, list]:
    tags = merged_tags(repo, clone, online)
    releases, dates = [], []
    for tag in sorted(tags, key=tag_key, reverse=True):
        src = Source(clone, repo, tags[tag], tag, online)
        date = src.date()
        dates.append(date)
        entry = {"version": tag[1:], "tag": tag, "commit": tags[tag], "kind": "rc" if "-rc." in tag else "release",
                 "date": date, "osv": None}
        entry.update(extra or {})
        if bindings:
            entry["bindings"] = kb_core_bindings(src)
            entry["language"] = kb_language(src.text("kb.yml"), f"{repo}@{tag}")
        releases.append(entry)
    return releases, dates


# -- the catalogue -------------------------------------------------------------------------------------------------

def build(drivers_root: Path, online: bool, figures: bool, include_head: bool = False) -> dict:
    platforms = json.loads((CONF / "data" / "platforms.json").read_text())
    try:
        rules = kb_tags.load_rules()
    except kb_tags.RulesError as exc:
        raise BuildError(str(exc)) from exc
    if not online:
        print("build_catalog: topics not fetched (no --online): tags come from the driver.yml fallback only; "
              "rerun with --online before committing the catalogue", file=sys.stderr)
    clones = sorted(p for p in drivers_root.glob("driver-*")
                    if p.is_dir() and (p / "driver.yml").exists() and not p.name.startswith("driver-template"))
    if not clones:
        raise BuildError(f"no driver clones under {drivers_root}")
    if figures:
        shutil.rmtree(CONF / "assets" / "figures", ignore_errors=True)
    drivers, dates = [], []
    for clone in clones:
        driver, d = build_driver(clone, online, figures, platforms, rules)
        drivers.append(driver)
        dates += d
    names = [d["name"] for d in drivers]
    if len(names) != len(set(names)):
        raise BuildError("two clones declare the same driver name")
    kb = platforms["kb_core"]
    kb_clone = CONF.parent if (CONF.parent / ".git").exists() else None
    kb_releases, d = build_releases(kb["repo"], kb_clone, online, {"bindings": []}, bindings=True)
    dates += d
    if include_head:
        if kb_clone is None:
            raise BuildError("--include-head needs the kb-core clone around configurator/")
        head = kb_core_head(kb_clone)
        if not any(r["commit"] == head["commit"] for r in kb_releases):
            kb_releases.insert(0, head)
            dates.append(head["date"])
    pins = driver_pins(kb_clone)
    for drv in drivers:
        drv["pinned"] = pins.get(drv["name"])
    families = []
    for fam in platforms["families"]:
        fam = dict(fam)
        if fam["port"] is not None:
            port = dict(fam["port"])
            # In-tree ports (the repository is kb-core itself) have no releases of their own.
            port["releases"], d = ([], []) if port["repo"] == kb["repo"] else build_releases(port["repo"], None, online, {"kb_core": ""})
            dates += d
            fam["port"] = port
        families.append(fam)
    catalog = {
        "schema": 1,
        "generator": "configurator/tools/build_catalog.py",
        "source_date": max(dates) if dates else "2026-01-01T00:00:00Z",
        "kb_core": {"repo": kb["repo"], "url": kb["url"], "parts": kb["parts"], "releases": kb_releases},
        "rtos": platforms["rtos"],
        "bus_kinds": platforms["bus_kinds"],
        "families": families,
        "drivers": sorted(drivers, key=lambda x: x["name"]),
        "tag_index": kb_tags.tag_index(drivers, rules),
        "examples": kb_examples.catalog_form(kb_examples.load(str(CONF.parent)), str(CONF.parent)),
        "tested_sets": [],
    }
    schema = json.loads((CONF / "schemas-draft" / "catalog.schema.json").read_text())
    errors = validate(catalog, schema)
    if errors:
        raise BuildError("catalogue does not match the schema:\n  " + "\n  ".join(errors[:40]))
    return catalog


def render(catalog: dict) -> tuple[str, str]:
    text = json.dumps(catalog, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
    js = ("// GENERATED by configurator/tools/build_catalog.py: do not edit. The same JSON as catalog.json, so the\n"
          "// page also works when opened from a file, where a fetch of a local file is refused.\n"
          "globalThis.KB_CATALOG = " + text.rstrip("\n") + ";\n")
    return text, js


TOPIC_KEYS = ("tags", "category", "tag_index")


def _released(obj, topics: bool = True):
    """`obj` without development revisions (dicts with kind "dev") and without the source date they move; without
    the topic-derived fields too when `topics` is false (an offline check cannot fetch them)."""
    if isinstance(obj, dict):
        return {k: _released(v, topics) for k, v in obj.items()
                if k != "source_date" and (topics or k not in TOPIC_KEYS)}
    if isinstance(obj, list):
        return [_released(v, topics) for v in obj if not (isinstance(v, dict) and v.get("kind") == "dev")]
    return obj


def _without_own_future(catalog: dict) -> dict:
    """`catalog` without the kb-core releases the checked commit cannot hold: a tag on that commit or after it.

    A release tag points at a commit whose catalogue was committed before the tag existed, so the check of that
    commit (the tag's own CI run) must not expect the tag in it.
    """
    clone = CONF.parent if (CONF.parent / ".git").exists() else None
    if clone is None:
        return catalog
    head = (run(["git", "rev-parse", "HEAD"], cwd=clone) or "").strip()

    def older(commit: str) -> bool:
        return commit != head and run(["git", "merge-base", "--is-ancestor", commit, head], cwd=clone,
                                      check=False) is not None

    out = dict(catalog)
    out["kb_core"] = dict(catalog["kb_core"])
    out["kb_core"]["releases"] = [r for r in catalog["kb_core"]["releases"] if r["kind"] == "dev" or older(r["commit"])]
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--drivers-root", type=Path, default=CONF.parent.parent,
                    help="folder holding the driver-<chip> clones (default: next to kb-core)")
    ap.add_argument("--online", action="store_true", help="also read tags and files from GitHub with gh")
    ap.add_argument("--include-head", action="store_true", help="add kb-core's HEAD as a development revision")
    ap.add_argument("--check", action="store_true", help="fail when the committed catalogue differs")
    ap.add_argument("--no-figures", action="store_true", help="do not copy figures (with --check)")
    args = ap.parse_args(argv)
    try:
        catalog = build(args.drivers_root, args.online, figures=not (args.no_figures or args.check),
                        include_head=args.include_head)
    except BuildError as exc:
        print(f"build_catalog: error: {exc}", file=sys.stderr)
        return 1
    text, js = render(catalog)
    out_json, out_js = CONF / "data" / "catalog.json", CONF / "data" / "catalog.js"
    if args.check:
        # The development revision (--include-head, kind dev) moves with every push to main, so the check compares the
        # released content only: dev revisions and the source date they feed are left out on both sides. A local
        # preview keeps --include-head; CI checks without it. A kb-core tag on the checked commit or a later one is
        # left out of the fresh build, since that commit's catalogue was written before the tag existed.
        committed = json.loads(out_json.read_text()) if out_json.exists() else None
        if not args.online:
            print("build_catalog: topics not compared (no --online); CI checks them with --check --online")
        same = committed is not None and (_released(committed, args.online)
                                          == _released(_without_own_future(json.loads(text)), args.online))
        print("build_catalog: up to date" if same else "build_catalog: catalogue differs; rerun the builder")
        return 0 if same else 1
    out_json.write_text(text)
    out_js.write_text(js)
    n = len(catalog["drivers"])
    nv = sum(len(d["versions"]) for d in catalog["drivers"])
    print(f"build_catalog: {n} drivers, {nv} versions, source date {catalog['source_date']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
