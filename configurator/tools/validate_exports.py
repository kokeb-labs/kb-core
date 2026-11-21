#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Validate what the configurator exported (tools/export_matrix.mjs OUT) with the official tools and kb's schemas.

For every exported row of OUT/summary.json:

* SPDX 2.3: ``evidence/sbom.spdx.json`` parsed and validated by the SPDX project's own library (spdx-tools);
* CycloneDX 1.6: ``evidence/sbom.cdx.json`` against the official 1.6 JSON schema, strict, through the CycloneDX
  project's library (cyclonedx-python-lib);
* ``kb-image.yml`` against catalog/schemas/kb-image.schema.json and ``kb-image.lock.yml`` against
  catalog/schemas/lock.schema.json (JSON Schema 2020-12);
* ``kb resolve`` (tools/kb, the command line) on the exported kb-image.yml with the page's catalogue writes the
  same lock, presets, kb_image.cmake and kb-fetch.sh, byte for byte;
* the licence report lists every component of the lock with the lock's licence, each licence a valid SPDX id, and
  the SBOMs name the same components.

Usage: validate_exports.py OUT        (needs PyYAML, jsonschema, spdx-tools and cyclonedx-python-lib[json-validation])
Exit 0 when every row passes; OUT/validation.json holds the results.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import yaml  # noqa: E402
from jsonschema import Draft202012Validator  # noqa: E402

from kb import catalog as kb_catalog  # noqa: E402
from kb import emit, resolver  # noqa: E402


def schema(name: str) -> Draft202012Validator:
    return Draft202012Validator(json.loads((ROOT / "catalog" / "schemas" / name).read_text(encoding="utf-8")))


def spdx_problems(path: pathlib.Path) -> list[str]:
    from spdx_tools.spdx.parser.parse_anything import parse_file
    from spdx_tools.spdx.validation.document_validator import validate_full_spdx_document

    doc = parse_file(str(path))
    return [m.validation_message for m in validate_full_spdx_document(doc, "SPDX-2.3")]


def cyclonedx_problems(path: pathlib.Path) -> list[str]:
    from cyclonedx.schema import OutputFormat, SchemaVersion
    from cyclonedx.validation import make_schemabased_validator

    validator = make_schemabased_validator(OutputFormat.JSON, SchemaVersion.V1_6)
    err = validator.validate_str(path.read_text(encoding="utf-8"))
    if err is None:
        return []
    errs = err if isinstance(err, (list, tuple)) else [err]
    return [str(getattr(e, "data", e))[:300] for e in errs]


SPDX_ID = re.compile(r"^[A-Za-z0-9.+-]+$")


def licence_problems(project: pathlib.Path, lock: dict) -> list[str]:
    text = (project / "evidence" / "licences.md").read_text(encoding="utf-8")
    rows = {}
    for line in text.splitlines():
        m = re.match(r"^\| (\S+) \| (\S+) \| (\S+) \| (.+) \|$", line)
        if m and m.group(1) not in ("Component", "---"):
            rows[m.group(1)] = (m.group(2), m.group(3))
    problems = []
    want = {"kb-core": (lock["kb_core"]["version"], lock["kb_core"]["license"])}
    for d in lock["drivers"]:
        want[d["folder"]] = (d["version"], d["license"])
    for name, (version, lic) in want.items():
        if name not in rows:
            problems.append(f"licences.md: {name} is missing")
        elif rows[name] != (version, lic):
            problems.append(f"licences.md: {name} says {rows[name]}, the lock {(version, lic)}")
    for name, (_, lic) in rows.items():
        if lic != "NOASSERTION" and not SPDX_ID.match(lic):
            problems.append(f"licences.md: {name}: {lic} is not an SPDX id")
    cdx = json.loads((project / "evidence" / "sbom.cdx.json").read_text(encoding="utf-8"))
    names = {c.get("name") for c in cdx.get("components", [])}
    for name in want:
        if name not in names:
            problems.append(f"sbom.cdx.json: no component {name}")
    spdx = json.loads((project / "evidence" / "sbom.spdx.json").read_text(encoding="utf-8"))
    pkgs = {p.get("name") for p in spdx.get("packages", [])}
    for name in want:
        if name not in pkgs:
            problems.append(f"sbom.spdx.json: no package {name}")
    return problems


def cli_problems(project: pathlib.Path, site: dict) -> list[str]:
    image = yaml.safe_load((project / "kb-image.yml").read_text(encoding="utf-8"))
    cat = kb_catalog.from_site(site)
    res = resolver.resolve(cat, image)
    if not res["ok"]:
        return [f"kb resolve: {e['code']} {e['subject']}: {e['message']}" for e in res["errors"]]
    problems = []
    for name, text in emit.files(res["lock"]).items():
        got = (project / name).read_text(encoding="utf-8")
        if got != text:
            problems.append(f"kb resolve writes a different {name} than the page")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    out = pathlib.Path(argv[1])
    summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    site = json.loads((ROOT / "configurator" / "data" / "catalog.json").read_text(encoding="utf-8"))
    image_schema, lock_schema = schema("kb-image.schema.json"), schema("lock.schema.json")
    results, failed = [], 0
    for row in summary["rows"]:
        project = out / row["id"] / "project"
        if not project.is_dir():
            results.append({"id": row["id"], "skipped": row.get("refused") and f"refused with {row['refused']}"})
            print(f"skip {row['id']}: {'refused with ' + row['refused'] if row.get('refused') else 'not exported'}")
            continue
        lock_text = (project / "kb-image.lock.yml").read_text(encoding="utf-8")
        lock = json.loads(lock_text.split("\n", 1)[1])
        image = yaml.safe_load((project / "kb-image.yml").read_text(encoding="utf-8"))
        checks = {
            "spdx-2.3 (spdx-tools)": spdx_problems(project / "evidence" / "sbom.spdx.json"),
            "cyclonedx-1.6 (cyclonedx-python-lib, strict schema)": cyclonedx_problems(project / "evidence" / "sbom.cdx.json"),
            "kb-image.yml schema": [f"{'/'.join(map(str, e.path))}: {e.message}" for e in image_schema.iter_errors(image)],
            "lock schema": [f"{'/'.join(map(str, e.path))}: {e.message}" for e in lock_schema.iter_errors(lock)],
            "kb resolve agrees": cli_problems(project, site),
            "licence report": licence_problems(project, lock),
        }
        ok = all(not v for v in checks.values())
        failed += not ok
        results.append({"id": row["id"], "ok": ok, "checks": {k: (v or "pass") for k, v in checks.items()}})
        print(f"{'ok  ' if ok else 'FAIL'} {row['id']}: " + ", ".join(f"{k} {'pass' if not v else 'FAIL'}" for k, v in checks.items()))
        for k, v in checks.items():
            for p in v[:8]:
                print(f"       {k}: {p}")
    (out / "validation.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory():
        sys.exit(main(sys.argv))
