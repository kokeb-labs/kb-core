#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Known vulnerabilities of the components this kb-core revision pins, from the OSV database (kb-vulns/1).

The components are the ones a release is built from: every pinned driver (catalog/compat.json, written from
components/drivers/drivers.lock.yml), the ThreadX kernel (the gitlink of third_party/threadx) and the ESP-IDF
release the ESP32 port builds against (tools/kb/sbom.py). Each is asked about by the commit of its pinned tag, so a
release candidate and a moved tag are judged by the code that is actually pinned.

Usage:
  scan.py --out configurator/data/vulnerabilities.json     scan (network: api.osv.dev, git ls-remote) and write
  scan.py --offline-fixture FILE --out FILE --date D       no network: answers come from the fixture (tests)
  scan.py --check [FILE]                                   only validate FILE against the schema and its own sums

Exit status: 0 done (advisories found are not an error here: the workflow reports them), 2 the scan itself failed
(network, an unreadable answer, a component that cannot be resolved). A failed scan never writes a file.

Standard library only. Schema: catalog/schemas/vulnerabilities.schema.json.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))

from kb import sbom  # noqa: E402  (tools/kb: the SOUP versions are written once, there)
from kb.tests import minischema  # noqa: E402  (pure standard library; the same validator the tests use)

SCANNER = "kb-vuln-scan/1"
STALE_AFTER_DAYS = 10
OSV = "https://api.osv.dev/v1"
DEFAULT_OUT = "configurator/data/vulnerabilities.json"
SCHEMA = "catalog/schemas/vulnerabilities.schema.json"
BATCH = 500  # OSV accepts up to 1000 queries per batch
NONE_KNOWN = "none known"
FOUND = "advisories found"


class ScanError(Exception):
    pass


# --- components -------------------------------------------------------------------------------------------------

def gitlink(root: pathlib.Path, path: str) -> str:
    """The commit a submodule is pinned at, read from the index (no checkout of the submodule is needed)."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-tree", "HEAD", path], capture_output=True, text=True,
                             check=True, timeout=60).stdout.split()
    except (OSError, subprocess.SubprocessError) as exc:
        raise ScanError(f"cannot read the pin of {path}: {exc}") from exc
    if len(out) < 3 or out[0] != "160000":
        raise ScanError(f"{path} is not a submodule of this revision")
    return out[2]


def remote_tag_commit(repo: str, tag: str) -> str:
    """The commit a tag of a public repository points at (the peeled one for an annotated tag)."""
    try:
        out = subprocess.run(["git", "ls-remote", f"https://github.com/{repo}", f"refs/tags/{tag}",
                              f"refs/tags/{tag}^{{}}"], capture_output=True, text=True, check=True,
                             timeout=120).stdout.split("\n")
    except (OSError, subprocess.SubprocessError) as exc:
        raise ScanError(f"cannot resolve {repo} {tag}: {exc}") from exc
    refs = {}
    for line in out:
        if line.strip():
            sha, ref = line.split()
            refs[ref] = sha
    sha = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
    if not sha:
        raise ScanError(f"{repo} has no tag {tag}")
    return sha


def components(root: pathlib.Path, resolve=remote_tag_commit, pin=gitlink) -> tuple[str, list[dict]]:
    """(kb-core version, the pinned components: name, version, purl, commit), sorted by name."""
    compat = json.loads((root / "catalog" / "compat.json").read_text(encoding="utf-8"))
    out = []
    for d in compat["drivers"]:
        repo = d["repo"]
        out.append({"name": repo.split("/")[-1], "version": d["tag"], "commit": d["commit"],
                    "purl": f"pkg:github/{repo}@{d['tag']}"})
    out.append({"name": "threadx", "version": sbom.THREADX_PIN, "commit": pin(root, "third_party/threadx"),
                "purl": f"pkg:github/eclipse-threadx/threadx@{sbom.THREADX_PIN}"})
    idf = sbom.soup({"mcu": {"family": "esp32"}, "rtos": "none"})
    for s in idf:
        if s["name"] == "esp-idf":
            out.append({"name": "esp-idf", "version": s["version"], "commit": resolve("espressif/esp-idf", s["version"]),
                        "purl": f"pkg:github/espressif/esp-idf@{s['version']}"})
    out.sort(key=lambda c: c["name"])
    return str(compat.get("kb_core", "0.0.0")), out


# --- OSV --------------------------------------------------------------------------------------------------------

class Osv:
    """The two OSV calls the scan needs, with a few retries for a busy server."""

    def __init__(self, base: str = OSV, tries: int = 4):
        self.base, self.tries = base, tries

    def _call(self, url: str, body: dict | None = None) -> dict:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json",
                                                              "User-Agent": SCANNER})
        last: Exception | None = None
        for attempt in range(self.tries):
            try:
                with urllib.request.urlopen(req, timeout=60) as resp:
                    return json.load(resp)
            except urllib.error.HTTPError as exc:
                last = exc
                if exc.code < 500 and exc.code != 429:
                    break
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
                last = exc
            time.sleep(2 ** attempt)
        raise ScanError(f"OSV {url}: {last}")

    def query_commits(self, commits: list[str]) -> dict[str, list[str]]:
        """commit -> advisory ids, following OSV's paging."""
        result: dict[str, list[str]] = {}
        for i in range(0, len(commits), BATCH):
            chunk = commits[i:i + BATCH]
            pending = [{"commit": c} for c in chunk]
            owners = list(chunk)
            ids = {c: [] for c in chunk}
            while pending:
                answer = self._call(f"{self.base}/querybatch", {"queries": pending})
                results = answer.get("results")
                if not isinstance(results, list) or len(results) != len(pending):
                    raise ScanError("OSV answered a batch with the wrong number of results")
                nxt, nxt_owners = [], []
                for owner, q, r in zip(owners, pending, results):
                    ids[owner] += [v["id"] for v in r.get("vulns", [])]
                    if r.get("next_page_token"):
                        nxt.append({**q, "page_token": r["next_page_token"]})
                        nxt_owners.append(owner)
                pending, owners = nxt, nxt_owners
            result.update({c: sorted(set(v)) for c, v in ids.items()})
        return result

    def vuln(self, vid: str) -> dict:
        return self._call(f"{self.base}/vulns/{vid}")


class Fixture:
    """Answers from a file, for tests: {"resolve": {"owner/repo@tag": sha}, "pins": {path: sha}, "commits": {sha: [id]}, "vulns": {id: doc}}."""

    def __init__(self, path: pathlib.Path):
        self.data = json.loads(path.read_text(encoding="utf-8"))

    def resolve(self, repo: str, tag: str) -> str:
        try:
            return self.data["resolve"][f"{repo}@{tag}"]
        except KeyError as exc:
            raise ScanError(f"fixture has no commit for {repo}@{tag}") from exc

    def pin(self, root: pathlib.Path, path: str) -> str:
        try:
            return self.data["pins"][path]
        except KeyError as exc:
            raise ScanError(f"fixture has no pin for {path}") from exc

    def query_commits(self, commits: list[str]) -> dict[str, list[str]]:
        return {c: sorted(self.data.get("commits", {}).get(c, [])) for c in commits}

    def vuln(self, vid: str) -> dict:
        try:
            return self.data["vulns"][vid]
        except KeyError as exc:
            raise ScanError(f"fixture has no advisory {vid}") from exc


# --- advisories -------------------------------------------------------------------------------------------------

_V3 = {"AV": {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2}, "AC": {"L": 0.77, "H": 0.44},
       "UI": {"N": 0.85, "R": 0.62}, "C": {"H": 0.56, "L": 0.22, "N": 0.0}}


def _roundup(x: float) -> float:
    i = round(x * 100000)
    return i / 100000.0 if i % 10000 == 0 else (math.floor(i / 10000) + 1) / 10.0


def cvss3_score(vector: str) -> float | None:
    """The CVSS 3.x base score of a vector, or None when it cannot be read."""
    try:
        m = dict(p.split(":") for p in vector.split("/")[1:])
        changed = m["S"] == "C"
        pr = {"N": 0.85, "L": 0.68 if changed else 0.62, "H": 0.5 if changed else 0.27}[m["PR"]]
        iss = 1 - (1 - _V3["C"][m["C"]]) * (1 - _V3["C"][m["I"]]) * (1 - _V3["C"][m["A"]])
        impact = 7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15 if changed else 6.42 * iss
        expl = 8.22 * _V3["AV"][m["AV"]] * _V3["AC"][m["AC"]] * pr * _V3["UI"][m["UI"]]
    except (KeyError, ValueError):
        return None
    if impact <= 0:
        return 0.0
    return _roundup(min(1.08 * (impact + expl), 10) if changed else min(impact + expl, 10))


def label(score: float) -> str:
    return ("none" if score == 0 else "low" if score < 4 else "medium" if score < 7 else "high" if score < 9
            else "critical")


def advisory(doc: dict) -> dict:
    """One advisory in the shape of the schema, from an OSV vulnerability document."""
    vid = doc["id"]
    severity, cvss = "unrated", None
    stated = (doc.get("database_specific") or {}).get("severity")
    if isinstance(stated, str) and stated.lower() in ("critical", "high", "medium", "low"):
        severity = stated.lower()
    elif isinstance(stated, str) and stated.lower() == "moderate":
        severity = "medium"
    for s in doc.get("severity", []):
        vec = s.get("score", "")
        if vec.startswith("CVSS:"):
            cvss = cvss or vec
            if severity == "unrated" and vec.startswith("CVSS:3"):
                score = cvss3_score(vec)
                if score is not None:
                    severity = label(score)
    text = doc.get("summary") or " ".join((doc.get("details") or "").split()) or "no summary published"
    if len(text) > 160:
        text = text[:157].rstrip() + "..."
    out = {"id": vid, "severity": severity, "summary": text, "url": f"https://osv.dev/vulnerability/{vid}"}
    if cvss:
        out["cvss"] = cvss
    return out


# --- scan -------------------------------------------------------------------------------------------------------

def scan(root: pathlib.Path, today: str, backend, resolve, pin=gitlink) -> dict:
    kb_version, comps = components(root, resolve=resolve, pin=pin)
    by_commit = backend.query_commits(sorted({c["commit"] for c in comps}))
    seen: dict[str, dict] = {}
    out = []
    for c in comps:
        advs = []
        for vid in by_commit.get(c["commit"], []):
            if vid not in seen:
                seen[vid] = advisory(backend.vuln(vid))
            advs.append(seen[vid])
        advs.sort(key=lambda a: a["id"])
        out.append({**c, "status": FOUND if advs else NONE_KNOWN, "count": len(advs), "advisories": advs})
    n = sum(c["count"] for c in out)
    return {
        "schema": "kb-vulns/1", "checked_at": today, "source": "OSV", "scanner": SCANNER,
        "stale_after_days": STALE_AFTER_DAYS, "kb_core": kb_version, "status": FOUND if n else NONE_KNOWN,
        "totals": {"components": len(out), "with_advisories": sum(1 for c in out if c["count"]), "advisories": n},
        "components": out,
    }


def dumps(doc: dict) -> str:
    return json.dumps(doc, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def problems(doc, root: pathlib.Path = ROOT) -> list[str]:
    """Schema violations plus the sums the schema cannot express."""
    schema = json.loads((root / SCHEMA).read_text(encoding="utf-8"))
    errs = minischema.errors(schema, doc)
    if errs:
        return errs
    out = []
    for c in doc["components"]:
        if c["count"] != len(c["advisories"]):
            out.append(f"{c['name']}: count {c['count']} but {len(c['advisories'])} advisories listed")
        if c["status"] != (FOUND if c["advisories"] else NONE_KNOWN):
            out.append(f"{c['name']}: status {c['status']!r} does not match its advisories")
        ids = [a["id"] for a in c["advisories"]]
        if ids != sorted(set(ids)):
            out.append(f"{c['name']}: advisories are not sorted and unique")
    t = doc["totals"]
    want = {"components": len(doc["components"]), "with_advisories": sum(1 for c in doc["components"] if c["count"]),
            "advisories": sum(c["count"] for c in doc["components"])}
    if t != want:
        out.append(f"totals {t} do not match the components {want}")
    if doc["status"] != (FOUND if want["advisories"] else NONE_KNOWN):
        out.append(f"status {doc['status']!r} does not match the totals")
    names = [c["name"] for c in doc["components"]]
    if names != sorted(set(names)):
        out.append("components are not sorted by name and unique")
    try:
        datetime.date.fromisoformat(doc["checked_at"])
    except ValueError:
        out.append(f"checked_at {doc['checked_at']!r} is not a date")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=pathlib.Path, default=ROOT)
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--offline-fixture", type=pathlib.Path, help="answer from this file instead of the network")
    ap.add_argument("--date", help="UTC date to record (default: today)")
    ap.add_argument("--check", nargs="?", const=DEFAULT_OUT, metavar="FILE",
                    help="only validate FILE (default %(const)s): schema, sums, ordering")
    args = ap.parse_args(argv)
    try:
        if args.check:
            path = args.root / args.check if not pathlib.Path(args.check).is_absolute() else pathlib.Path(args.check)
            errs = problems(json.loads(path.read_text(encoding="utf-8")), args.root)
            for e in errs:
                print(f"scan: {path}: {e}")
            if not errs:
                print(f"scan: {path} is valid")
            return 1 if errs else 0
        today = args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        if args.offline_fixture:
            backend = Fixture(args.offline_fixture)
            resolve, pin = backend.resolve, backend.pin
        else:
            backend, resolve, pin = Osv(), remote_tag_commit, gitlink
        doc = scan(args.root, today, backend, resolve, pin)
        errs = problems(doc, args.root)
        if errs:
            raise ScanError("the result is not valid: " + "; ".join(errs))
        out = args.out or (args.root / DEFAULT_OUT)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(dumps(doc), encoding="utf-8")
        t = doc["totals"]
        print(f"scan: {out}: {t['components']} components, {t['with_advisories']} with advisories, "
              f"{t['advisories']} advisories (checked {today})")
        for c in doc["components"]:
            if c["count"]:
                print(f"scan:   {c['name']} {c['version']}: " + ", ".join(a["id"] for a in c["advisories"]))
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as fh:
                fh.write(f"advisories={t['advisories']}\ncomponents_with_advisories={t['with_advisories']}\n")
        return 0
    except ScanError as exc:
        print(f"scan: FAILED: {exc}")
        return 2
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        print(f"scan: FAILED: {type(exc).__name__}: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
