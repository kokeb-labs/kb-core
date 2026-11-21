#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Normalise the CI artifacts and release assets of one kb-core release into one ``kb-evidence/1`` document.

Reads only what CI and the release left behind, never the code or a live service:

* ``ARTIFACTS/<artifact>/...``: the artifacts of the CI run of the release tag (``gh run download RUN -D ARTIFACTS``):
  ``cmake-matrix-*`` (unit tests per part), ``host-quality-reports-coverage`` (gcovr summary),
  ``host-quality-reports-complexity`` (lizard), ``mutation-report``, ``misra-report``, ``pin-hygiene-report``,
  ``catalog`` (OSV scan, SBOMs), ``cppcheck-report``, ``layering-report``, ``lighthouse-report``;
* ``ARTIFACTS/run.json``: the run's own record (``gh run view RUN --json databaseId,url,conclusion,headSha,
  headBranch,createdAt,jobs``), for the job and step results that leave no file (sanitizers, fuzzing, link check);
* ``ASSETS/``: the release assets (``SHA256SUMS``, the SPDX SBOM).

Usage:
  collect.py --artifacts DIR --assets DIR --version 0.1.0-rc.1 -o evidence.json
  collect.py --site configurator/data              rebuild evidence.js (the page's form of every evidence/*.json)
  collect.py --site configurator/data --check      fail when evidence.js is stale

The output has sorted keys and no clock: the dates are the run's. Schema: catalog/schemas/evidence.schema.json.
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import statistics
import sys
import textwrap
import xml.etree.ElementTree as ET
from datetime import datetime

PARTS = ("utils", "rtos", "dma", "interfaces", "handlers")
PART_PATHS = {p: f"components/lib/{p}/" for p in PARTS}
LIGHTHOUSE_BUDGETS = {"performance": 0.8, "accessibility": 0.95, "best-practices": 0.9, "seo": 0.9}
#: ru-host-quality's limits for level high (ci-tools, ADR-0020 of the handbook): fuzz seconds per target, floors.
LEVEL_HIGH = {"fuzz_seconds": 120, "min_line": 85, "min_branch": 75, "min_condition": "report", "ccn": 15,
              "length": 60, "params": 6}


class EvidenceError(Exception):
    pass


def load(path: pathlib.Path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def one(root: pathlib.Path, pattern: str) -> pathlib.Path | None:
    hits = sorted(root.glob(pattern))
    return hits[0] if hits else None


def pct(covered: int, total: int) -> float:
    return round(100.0 * covered / total, 1) if total else 100.0


def seconds(a: str | None, b: str | None) -> int | None:
    if not a or not b:
        return None
    t0 = datetime.fromisoformat(a.replace("Z", "+00:00"))
    t1 = datetime.fromisoformat(b.replace("Z", "+00:00"))
    return max(0, int((t1 - t0).total_seconds()))


def jobs(run: dict) -> dict[str, dict]:
    return {j["name"]: j for j in run.get("jobs", [])}


def step(run: dict, job_part: str, step_name: str) -> dict | None:
    for name, job in jobs(run).items():
        if job_part in name:
            for s in job.get("steps", []):
                if s["name"] == step_name:
                    return s
    return None


def job_result(run: dict, job_part: str) -> str:
    for name, job in jobs(run).items():
        if job_part in name and job.get("conclusion") not in (None, "skipped"):
            return "pass" if job["conclusion"] == "success" else "fail"
    return "not run"


def tests(art: pathlib.Path) -> dict:
    parts = []
    for p in PARTS:
        f = one(art, f"cmake-matrix-*-{p}/result.json")
        if f is None:
            continue
        r = load(f)
        parts.append({"part": p, "tests": r["tests"]["tests"], "failures": r["tests"]["failures"],
                      "skipped": r["tests"]["skipped"], "seconds": r.get("seconds"), "status": r["status"],
                      "toolchain": r.get("toolchain")})
    if not parts:
        raise EvidenceError("no cmake-matrix-*/result.json: the unit test results are missing")
    return {"source": "cmake-matrix-*/result.json (Standalone parts: each part built and tested alone)",
            "total": sum(p["tests"] for p in parts), "failures": sum(p["failures"] for p in parts),
            "skipped": sum(p["skipped"] for p in parts), "parts": parts}


def coverage(art: pathlib.Path) -> dict:
    f = one(art, "host-quality-reports-coverage/coverage-summary.json")
    if f is None:
        raise EvidenceError("no host-quality-reports-coverage/coverage-summary.json")
    d = load(f)

    def block(src: dict, kind: str) -> dict:
        return {"covered": src[f"{kind}_covered"], "total": src[f"{kind}_total"],
                "percent": pct(src[f"{kind}_covered"], src[f"{kind}_total"])}

    parts = []
    for p, prefix in PART_PATHS.items():
        files = [x for x in d.get("files", []) if x["filename"].startswith(prefix)]
        if not files:
            continue
        agg = {f"{k}_{m}": sum(x.get(f"{k}_{m}", 0) for x in files) for k in ("line", "branch", "condition", "function")
               for m in ("covered", "total")}
        parts.append({"part": p, "files": len(files), **{k: block(agg, k)["percent"] for k in
                                                         ("line", "branch", "condition", "function")}})
    def pv(x: dict, kind: str) -> float:  # gcovr writes null for a file with nothing of that kind
        v = x.get(f"{kind}_percent")
        return 100.0 if v is None else v

    uncovered = sorted(
        ({"file": x["filename"], "line": pv(x, "line"), "branch": pv(x, "branch"), "condition": pv(x, "condition")}
         for x in d.get("files", []) if min(pv(x, "line"), pv(x, "branch"), pv(x, "condition")) < 100),
        key=lambda r: (r["branch"], r["condition"], r["file"]))
    return {"source": "host-quality-reports-coverage/coverage-summary.json (gcovr, GCC 14, library code only)",
            "line": block(d, "line"), "branch": block(d, "branch"), "condition": block(d, "condition"),
            "function": block(d, "function"),
            "condition_kind": "MC/DC (GCC -fcondition-coverage)",
            "floors": {"line": LEVEL_HIGH["min_line"], "branch": LEVEL_HIGH["min_branch"],
                       "condition": LEVEL_HIGH["min_condition"]},
            "files": len(d.get("files", [])), "parts": parts, "below_full": uncovered[:40]}


def mutation(art: pathlib.Path) -> dict:
    res = one(art, "mutation-report/mutation-results.json")
    md = one(art, "mutation-report/mutation-report.md")
    if res is None:
        raise EvidenceError("no mutation-report/mutation-results.json")
    rows = load(res)
    count: dict[str, int] = {}
    for m in rows:
        count[m["status"]] = count.get(m["status"], 0) + 1
    killed, survived = count.get("killed", 0), count.get("survived", 0)
    threshold = None
    if md is not None:
        m = re.search(r"threshold (\d+(?:\.\d+)?) %", md.read_text(encoding="utf-8"))
        threshold = float(m.group(1)) if m else None
    parts = []
    for p, prefix in PART_PATHS.items():
        mine = [m for m in rows if m["file"].startswith(prefix)]
        k = sum(1 for m in mine if m["status"] == "killed")
        s = sum(1 for m in mine if m["status"] == "survived")
        if mine:
            parts.append({"part": p, "killed": k, "survived": s, "score": pct(k, k + s),
                          "equivalent": sum(1 for m in mine if m["status"] == "equivalent")})
    return {"source": "mutation-report/mutation-results.json (kb-core's mutation tool, four shards)",
            "score": pct(killed, killed + survived), "threshold": threshold, "killed": killed, "survived": survived,
            "equivalent": count.get("equivalent", 0), "not_compiled": count.get("compile_error", 0) +
            count.get("not_compiled", 0) + count.get("did_not_compile", 0), "statuses": dict(sorted(count.items())),
            "parts": parts}


def misra(art: pathlib.Path) -> dict:
    f = one(art, "misra-report/misra-summary.json")
    if f is None:
        raise EvidenceError("no misra-report/misra-summary.json")
    d = load(f)
    t = d["totals"]
    configs = [{"name": c["name"], "translation_units": c["extraction"]["translation_units"],
                "source_files": c["extraction"]["analysed_source_files"], "complete": c["extraction"]["complete"]}
               for c in d.get("configurations", [])]
    guidelines = d.get("guidelines", {})
    by_cat: dict[str, int] = {}
    for g in guidelines.values():
        by_cat[g.get("category", "?")] = by_cat.get(g.get("category", "?"), 0) + 1
    return {"source": "misra-report/misra-summary.json (CodeQL MISRA C++:2023 queries, cppcheck, clang-tidy)",
            "edition": "MISRA C++:2023", "level": d.get("level"), "passed": bool(d.get("passed")),
            "configurations": configs, "guidelines": {"checked": len(guidelines), "by_category": dict(sorted(by_cat.items())),
            "clean": t["guidelines_clean"], "with_findings": t["guidelines_with_violations"],
            "deviated": t["guidelines_deviated"]},
            "findings": {"violations": t["violations"], "deviated": t["deviated"], "review": t["review"]},
            "failing_guidelines": list(d.get("failing_guidelines", [])),
            "advisory_with_findings": list(d.get("warning_guidelines", [])),
            "deviation_records": sorted(d.get("records", {}))}


def fuzz(art: pathlib.Path, run: dict) -> dict:
    lay = art / "layering-report"
    targets = sorted({m.group(1) for p in lay.glob("targets.dot.fuzz_replay_*")
                      if (m := re.match(r"targets\.dot\.fuzz_replay_(fuzz_[a-z0-9_]+?)(\.dependers)?$", p.name))})
    s = step(run, "fuzz (libFuzzer", "Fuzz")
    return {"source": "run.json (job fuzz, step Fuzz) and layering-report (the fuzz targets CMake defines)",
            "engine": "libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer (clang 18)",
            "targets": targets, "seconds_per_target": LEVEL_HIGH["fuzz_seconds"],
            "planned_seconds": LEVEL_HIGH["fuzz_seconds"] * len(targets),
            "step_seconds": seconds(s.get("startedAt"), s.get("completedAt")) if s else None,
            "result": "pass" if s and s.get("conclusion") == "success" else ("fail" if s else "not run")}


def sanitizers(run: dict) -> dict:
    s = step(run, "complexity, coverage, sanitizers", "Test under the sanitizers")
    return {"source": "run.json (job complexity, coverage, sanitizers, step Test under the sanitizers)",
            "kinds": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "compiler": "clang",
            "result": "pass" if s and s.get("conclusion") == "success" else ("fail" if s else "not run")}


def lighthouse(art: pathlib.Path) -> dict:
    pages: dict[str, dict[str, list[float]]] = {}
    for f in sorted((art / "lighthouse-report").glob("*.report.json")):
        d = load(f)
        url = d.get("finalDisplayedUrl") or d.get("finalUrl") or ""
        page = "/" + url.split("/", 3)[3] if url.count("/") >= 3 else url
        for k, v in d["categories"].items():
            pages.setdefault(page, {}).setdefault(k, []).append(v["score"])
    out = []
    for page in sorted(pages):
        med = {k: statistics.median(v) for k, v in pages[page].items()}
        out.append({"page": page, "runs": len(next(iter(pages[page].values()))),
                    **{k.replace("-", "_"): round(v, 2) for k, v in sorted(med.items())},
                    "pass": all(med.get(k, 0) >= b for k, b in LIGHTHOUSE_BUDGETS.items())})
    return {"source": "lighthouse-report/*.report.json (mobile emulation, median of three runs per page)",
            "budgets": {k.replace("-", "_"): v for k, v in LIGHTHOUSE_BUDGETS.items()}, "pages": out}


def statics(art: pathlib.Path) -> dict:
    out = {}
    x = one(art, "cppcheck-report/cppcheck_report.xml")
    if x is not None:
        errs = ET.parse(x).getroot().findall(".//error")
        sev: dict[str, int] = {}
        for e in errs:
            sev[e.get("severity", "?")] = sev.get(e.get("severity", "?"), 0) + 1
        out["cppcheck"] = {"findings": len(errs), "by_severity": dict(sorted(sev.items()))}
    over = one(art, "host-quality-reports-complexity/over.txt")
    if over is not None:
        lines = [ln for ln in over.read_text(encoding="utf-8").splitlines() if ln.strip()]
        out["complexity"] = {"over_limit": len(lines), "limits": {"ccn": LEVEL_HIGH["ccn"], "length": LEVEL_HIGH["length"],
                                                                 "params": LEVEL_HIGH["params"]}}
    lay = one(art, "layering-report/layering.sarif")
    if lay is not None:
        runs = load(lay).get("runs", [])
        out["layering"] = {"findings": sum(len(r.get("results", [])) for r in runs)}
    return out


def supply(art: pathlib.Path, assets: pathlib.Path | None) -> dict:
    out: dict = {}
    pins = one(art, "pin-hygiene-report/pins.json")
    if pins is not None:
        p = load(pins)["pins"]
        out["pins"] = {"drivers": len(p), "ok": sum(1 for x in p if x.get("ok")),
                       "kinds": dict(sorted({k: sum(1 for x in p if x.get("kind") == k) for k in {x.get("kind") for x in p}}.items()))}
    osv = one(art, "catalog/osv.json")
    if osv is not None:
        out["vulnerabilities"] = {"scanner": "osv-scanner (Catalogue job)", "findings": len(load(osv).get("results", []))}
    if assets is not None and assets.is_dir():
        sums = assets / "SHA256SUMS"
        files = []
        if sums.exists():
            for line in sums.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    digest, name = line.split(None, 1)
                    files.append({"name": name.strip().lstrip("*"), "sha256": digest})
        spdx = one(assets, "*.spdx.json")
        out["release_assets"] = files
        if spdx is not None:
            out["sbom"] = {"format": "SPDX 2.3", "file": spdx.name, "packages": len(load(spdx).get("packages", []))}
    return out


def protocol(ev: dict, run: dict) -> list[dict]:
    """What CI checks for every release, each with its tool, gate and this release's result."""
    t, c, m, mi = ev["tests"], ev["coverage"], ev["mutation"], ev["misra"]
    lh = ev["lighthouse"]["pages"]
    rows = [
        ("unit-tests", "Unit tests", "every part built and tested alone, GCC", "CTest", "every test passes",
         f"{t['total']} tests, {t['failures']} failed", t["failures"] == 0),
        ("coverage", "Coverage", "library code, line, branch and MC/DC", "gcovr, GCC 14",
         f"line {c['floors']['line']} %, branch {c['floors']['branch']} %, MC/DC reported",
         f"line {c['line']['percent']} %, branch {c['branch']['percent']} %, MC/DC {c['condition']['percent']} %",
         c["line"]["percent"] >= c["floors"]["line"] and c["branch"]["percent"] >= c["floors"]["branch"]),
        ("mutation", "Mutation testing", "every mutant of the library code", "kb-core mutation tool",
         f"kill rate {m['threshold']:g} %" if m["threshold"] is not None else "kill rate",
         f"{m['score']} % ({m['killed']} killed, {m['survived']} survived, {m['equivalent']} equivalent)",
         m["threshold"] is None or m["score"] >= m["threshold"]),
        ("misra", "MISRA C++:2023", "host and Cortex-M configurations", "CodeQL, cppcheck, clang-tidy",
         f"level {mi['level']}: no required finding outside a deviation",
         f"{mi['guidelines']['checked']} guidelines; {mi['findings']['violations']} findings in "
         f"{len(mi['advisory_with_findings'])} advisory guidelines, {mi['findings']['deviated']} deviated", mi["passed"]),
        ("fuzz", "Fuzzing", f"{len(ev['fuzz']['targets'])} targets", "libFuzzer, ASan, UBSan",
         f"{ev['fuzz']['seconds_per_target']} s per target, no crash",
         f"{ev['fuzz']['planned_seconds']} s planned, step ran {ev['fuzz']['step_seconds']} s",
         ev["fuzz"]["result"] == "pass"),
        ("sanitizers", "Sanitizers", "the unit tests", "ASan, UBSan (clang)", "every test passes", ev["sanitizers"]["result"],
         ev["sanitizers"]["result"] == "pass"),
        ("lighthouse", "Docs: Lighthouse", f"{len(lh)} pages, mobile", "Lighthouse CI",
         "performance 0.8, accessibility 0.95, best practices 0.9, SEO 0.9",
         ", ".join(f"{p['page']} {p['performance']:.2f}/{p['accessibility']:.2f}" for p in lh), all(p["pass"] for p in lh)),
        ("links", "Docs: links", "the built site, offline", "lychee", "no broken link", ev["links"]["result"],
         ev["links"]["result"] == "pass"),
    ]
    if "pins" in ev["supply"]:
        p = ev["supply"]["pins"]
        rows.append(("pins", "Driver pins", "every submodule", "ru-pin-hygiene", "release or rc tags only, CI passed",
                     f"{p['ok']} of {p['drivers']} pins valid", p["ok"] == p["drivers"]))
    if "vulnerabilities" in ev["supply"]:
        v = ev["supply"]["vulnerabilities"]
        rows.append(("vulnerabilities", "Known vulnerabilities", "kb-core and its pinned parts", "OSV scanner",
                     "reported", f"{v['findings']} known", v["findings"] == 0))
    return [{"id": i, "check": n, "scope": s, "tool": tool, "gate": g, "result": r, "status": "pass" if ok else "fail"}
            for i, n, s, tool, g, r, ok in rows]


def collect(art: pathlib.Path, assets: pathlib.Path | None, version: str) -> dict:
    run = load(art / "run.json") if (art / "run.json").exists() else {}
    tag = f"v{version}"
    if run and run.get("headBranch") not in (tag, None):
        raise EvidenceError(f"run.json is the run of {run.get('headBranch')}, not of {tag}")
    links = step(run, "Docs / docs / Build and check", "Check links on the built site (offline)")
    ev = {
        "schema": "kb-evidence/1",
        "release": {"version": version, "tag": tag, "commit": run.get("headSha"), "kind": "rc" if "-rc." in version else "release"},
        "source": {"run_id": run.get("databaseId"), "run_url": run.get("url"), "conclusion": run.get("conclusion"),
                   "date": run.get("createdAt"),
                   "artifacts": sorted(p.name for p in art.iterdir() if p.is_dir())},
        "tests": tests(art),
        "coverage": coverage(art),
        "mutation": mutation(art),
        "misra": misra(art),
        "fuzz": fuzz(art, run),
        "sanitizers": sanitizers(run),
        "lighthouse": lighthouse(art),
        "links": {"source": "run.json (job Docs, step Check links on the built site)", "tool": "lychee, offline",
                  "result": "pass" if links and links.get("conclusion") == "success" else ("fail" if links else "not run")},
        "static_analysis": statics(art),
        "supply": supply(art, assets),
    }
    ev["protocol"] = protocol(ev, run)
    ev["passed"] = all(r["status"] == "pass" for r in ev["protocol"])
    return ev


def render_site(data: pathlib.Path) -> tuple[str, str]:
    docs = {}
    for f in sorted((data / "evidence").glob("*.json")):
        d = load(f)
        docs[d["release"]["version"]] = d
    text = json.dumps(docs, indent=1, sort_keys=True, ensure_ascii=False)
    js = ("// SPDX-License-Identifier: MIT\n// Copyright (c) 2026 The kb-core authors\n"
          "// GENERATED by tools/evidence/collect.py --site: do not edit. Every data/evidence/<version>.json, keyed by\n"
          "// version, as a script so the page also works from a file.\n"
          f"globalThis.KB_EVIDENCE = {text};\n")
    return text, js


def _cell(v) -> str:
    return str(v).replace("|", "\\|")


def render_docs(data: pathlib.Path) -> str:
    """docs/assurance/evidence.md: every release's protocol and results as tables (the same data as the page)."""
    docs = [load(f) for f in sorted((data / "evidence").glob("*.json"))]
    docs.sort(key=lambda d: [int(x) if x.isdigit() else x for x in re.split(r"[.-]", d["release"]["version"])],
              reverse=True)
    out = [
        "# Test evidence",
        "",
        "<!-- GENERATED by tools/evidence/collect.py --site configurator/data --docs docs/assurance/evidence.md: do not",
        "edit; the data is configurator/data/evidence/<version>.json (schema catalog/schemas/evidence.schema.json). -->",
        "",
        "What CI tested for each kb-core release, and what it found: the test protocol (every check, its tool and its",
        "gate) and the results, read from the CI artifacts and release assets of the release tag alone. The",
        "[configurator](../app/index.html) shows the same data, with charts, for the kb-core version you select.",
        "",
        "After each release, `gh run download <run of the tag> -D artifacts`, `gh run view <run> --json databaseId,url,"
        "conclusion,headSha,headBranch,createdAt,jobs > artifacts/run.json` and `gh release download <tag> -D assets` "
        "feed `tools/evidence/collect.py --artifacts artifacts --assets assets --version <version> -o "
        "configurator/data/evidence/<version>.json`; then `--site configurator/data --docs docs/assurance/evidence.md` "
        "rewrites this page and the page's data, and CI checks both are current.",
        "",
        "These are the results of the checks; they are not a safety claim ([assurance level](../adr/ADR-0011-assurance-level-high.md)).",
        "",
    ]
    for d in docs:
        r, src = d["release"], d["source"]
        out += [f"## {r['version']}", "",
                f"Tag `{r['tag']}` at `{(r['commit'] or '?')[:12]}`, CI run [{src['run_id']}]({src['run_url']}) "
                f"({src['conclusion']}, {(src.get('date') or '')[:10]}). "
                f"**{'Every check passed.' if d['passed'] else 'Not every check passed.'}**", "",
                "### Protocol and results", "",
                "| Check | Scope | Tool | Gate | Result | Status |", "| --- | --- | --- | --- | --- | --- |"]
        for p in d["protocol"]:
            out.append(f"| {_cell(p['check'])} | {_cell(p['scope'])} | {_cell(p['tool'])} | {_cell(p['gate'])} | "
                       f"{_cell(p['result'])} | {p['status']} |")
        cov, mut, t = d["coverage"], d["mutation"], d["tests"]
        mut_parts = {m["part"]: m for m in mut.get("parts", [])}
        test_parts = {x["part"]: x for x in t["parts"]}
        out += ["", "### By part", "",
                "| Part | Unit tests | Line | Branch | MC/DC | Functions | Mutants killed | Survived | Kill rate |",
                "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
        for c in cov["parts"]:
            m = mut_parts.get(c["part"], {})
            out.append(f"| {c['part']} | {test_parts.get(c['part'], {}).get('tests', '-')} | {c['line']} % | "
                       f"{c['branch']} % | {c['condition']} % | {c['function']} % | {m.get('killed', '-')} | "
                       f"{m.get('survived', '-')} | {str(m.get('score', '-')) + ' %' if m else '-'} |")
        out.append(f"| **all** | **{t['total']}** | **{cov['line']['percent']} %** | **{cov['branch']['percent']} %** | "
                   f"**{cov['condition']['percent']} %** | **{cov['function']['percent']} %** | **{mut['killed']}** | "
                   f"**{mut['survived']}** | **{mut['score']} %** |")
        out += ["", f"Coverage: {cov['source']}; MC/DC is {cov['condition_kind']}. Mutation: {mut['equivalent']} "
                f"equivalent mutants (each with its reason in `docs/assurance/equivalent-mutants.yml`), "
                f"{mut.get('not_compiled', 0)} that did not compile (not scored).", ""]
        if cov.get("below_full"):
            out += ["Files below 100 % on any measure:", "", "| File | Line | Branch | MC/DC |", "| --- | --- | --- | --- |"]
            out += [f"| `{b['file']}` | {b['line']} % | {b['branch']} % | {b['condition']} % |" for b in cov["below_full"]]
            out.append("")
        mi = d["misra"]
        out += ["### MISRA C++:2023", "",
                f"Level {mi['level']}, {'passed' if mi['passed'] else 'failed'}: {mi['guidelines']['checked']} guidelines "
                f"({', '.join(f'{v} {k}' for k, v in mi['guidelines']['by_category'].items())}); "
                f"{mi['guidelines']['clean']} clean, {mi['guidelines']['with_findings']} with findings, "
                f"{mi['guidelines']['deviated']} deviated. Findings: {mi['findings']['violations']} "
                f"(advisory guidelines {', '.join(mi['advisory_with_findings']) or 'none'}), "
                f"{mi['findings']['deviated']} under a deviation, {mi['findings']['review']} for review. "
                f"Deviation records: {', '.join(mi['deviation_records'])} ([deviations](../deviations.md)).", ""]
        if mi.get("configurations"):
            out += ["| Configuration | Translation units | Source files | Complete |", "| --- | --- | --- | --- |"]
            out += [f"| {c['name']} | {c['translation_units']} | {c['source_files']} | {'yes' if c['complete'] else 'no'} |"
                    for c in mi["configurations"]]
            out.append("")
        fz = d["fuzz"]
        out += ["### Fuzzing and sanitizers", "",
                f"{fz['engine']}: {len(fz['targets'])} targets ({', '.join(f'`{x}`' for x in fz['targets'])}), "
                f"{fz['seconds_per_target']} s each ({fz['planned_seconds']} s planned; the step ran "
                f"{fz['step_seconds']} s): {fz['result']}. Sanitizers ({', '.join(d['sanitizers']['kinds'])}) over the "
                f"unit tests: {d['sanitizers']['result']}.", "",
                "### Documentation site", "",
                "| Page | Performance | Accessibility | Best practices | SEO | Within budget |",
                "| --- | --- | --- | --- | --- | --- |"]
        out += [f"| `{p['page']}` | {p['performance']:.2f} | {p['accessibility']:.2f} | {p['best_practices']:.2f} | "
                f"{p['seo']:.2f} | {'yes' if p['pass'] else 'no'} |" for p in d["lighthouse"]["pages"]]
        b = d["lighthouse"]["budgets"]
        out += ["", f"Budgets: performance {b['performance']}, accessibility {b['accessibility']}, best practices "
                f"{b['best_practices']}, SEO {b['seo']} (median of three mobile runs). Links: {d['links']['tool']}, "
                f"{d['links']['result']}.", "", "### Supply chain and static analysis", ""]
        sup, st = d["supply"], d["static_analysis"]
        items = []
        if "pins" in sup:
            items.append(f"- Driver pins: {sup['pins']['ok']} of {sup['pins']['drivers']} valid (release or rc tags whose CI passed).")
        if "vulnerabilities" in sup:
            items.append(f"- Known vulnerabilities: {sup['vulnerabilities']['findings']} ({sup['vulnerabilities']['scanner']}).")
        if "sbom" in sup:
            items.append(f"- SBOM: {sup['sbom']['format']}, `{sup['sbom']['file']}`, {sup['sbom']['packages']} packages.")
        for a in sup.get("release_assets", []):
            items.append(f"- Release asset `{a['name']}`, SHA-256 `{a['sha256']}`.")
        if "complexity" in st:
            lim = st["complexity"]["limits"]
            items.append(f"- Complexity: {st['complexity']['over_limit']} functions over the limits (CCN {lim['ccn']}, "
                         f"{lim['length']} lines, {lim['params']} parameters).")
        if "cppcheck" in st:
            items.append(f"- cppcheck: {st['cppcheck']['findings']} messages "
                         f"({', '.join(f'{v} {k}' for k, v in st['cppcheck']['by_severity'].items())}).")
        if "layering" in st:
            items.append(f"- Layering: {st['layering']['findings']} findings.")
        out += items + [""]
    wrapped = []
    for line in out:  # prose at the 120-column limit of the docs; tables, headings and comments stay as they are
        if line.startswith(("|", "#", "<!--", "- ")) or len(line) <= 120:
            wrapped.append(line)
        else:
            wrapped += textwrap.wrap(line, 120, break_long_words=False, break_on_hyphens=False)
    return "\n".join(wrapped).rstrip("\n") + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--artifacts", type=pathlib.Path)
    ap.add_argument("--assets", type=pathlib.Path)
    ap.add_argument("--version")
    ap.add_argument("-o", "--output", type=pathlib.Path)
    ap.add_argument("--site", type=pathlib.Path, help="the configurator's data folder: rebuild evidence.js")
    ap.add_argument("--docs", type=pathlib.Path, help="with --site: also write (or check) this Markdown page")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.site:
            outputs = [(args.site / "evidence.js", render_site(args.site)[1])]
            if args.docs:
                outputs.append((args.docs, render_docs(args.site)))
            stale = [t for t, text in outputs if not t.exists() or t.read_text(encoding="utf-8") != text]
            if args.check:
                for t in stale:
                    print(f"collect: {t} is stale; run tools/evidence/collect.py --site {args.site}"
                          + (f" --docs {args.docs}" if args.docs else ""))
                if not stale:
                    print("collect: evidence outputs up to date")
                return 1 if stale else 0
            for t, text in outputs:
                t.write_text(text, encoding="utf-8")
                print(f"collect: wrote {t} ({len(text.encode()) // 1024} KiB)")
            return 0
        if not (args.artifacts and args.version and args.output):
            ap.error("--artifacts, --version and -o are needed (or --site)")
        ev = collect(args.artifacts, args.assets, args.version)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(ev, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"collect: {args.output}: {'passed' if ev['passed'] else 'FAILED'}, "
              + ", ".join(f"{r['id']} {r['status']}" for r in ev["protocol"]))
        return 0
    except EvidenceError as exc:
        print(f"collect: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
