#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The example index (examples/examples.yml) and the checks that keep everything else in step with it.

  examples.py check            fail on any mismatch between the index and the folders, the README table, the
                               documentation snippets, the ESP-IDF app matrices and the configurator catalogue
  examples.py write            regenerate the table in examples/README.md from the index
  examples.py json             the index as the JSON the configurator catalogue carries

Folders come from tools/layout/paths.yml (examples, ports, docs); nothing here spells out where they are.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "layout"))
import layout  # noqa: E402

KINDS = ("host", "esp-idf")
FIELDS = {"id", "title", "kind", "parts", "source", "summary", "doc", "snippets", "port"}
TABLE_BEGIN, TABLE_END = "<!-- examples:begin -->", "<!-- examples:end -->"
SNIPPET_INCLUDE = re.compile(r'--8<--\s+"([^"]+?)(?::([A-Za-z0-9_-]+))?"')
SNIPPET_START = re.compile(r"--8<--\s+\[start:([A-Za-z0-9_-]+)\]")


def _scalar(value: str):
    value = re.sub(r"\s+#.*$", "", value).strip()
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        return [v.strip() for v in inner.split(",")] if inner else []
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def parse(text: str) -> list[dict]:
    """The flat list-of-records subset the index uses (needs no PyYAML, like the rest of the layout tools)."""
    records: list[dict] = []
    for number, raw in enumerate(text.splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#") or raw.strip() == "---":
            continue
        m = re.match(r"^(- |  )([A-Za-z_]+):\s*(.*)$", raw)
        if not m:
            raise ValueError(f"examples.yml:{number}: cannot parse {raw!r}")
        if m.group(1) == "- ":
            records.append({})
        if not records:
            raise ValueError(f"examples.yml:{number}: a field before the first '- '")
        records[-1][m.group(2)] = _scalar(m.group(3))
    return records


def folders(root: str = ROOT) -> dict:
    data = layout.load(os.path.join(root, "tools", "layout", "paths.yml"))
    return {name: data["layout"][name] for name in ("examples", "ports", "docs")}


def index_path(root: str = ROOT) -> str:
    return os.path.join(root, folders(root)["examples"], "examples.yml")


def load(root: str = ROOT) -> list[dict]:
    with open(index_path(root), encoding="utf-8") as f:
        return parse(f.read())


def example_dir(rec: dict, root: str = ROOT) -> str:
    """The folder holding an example's source, relative to the repository root."""
    f = folders(root)
    if rec["kind"] == "host":
        return f"{f['examples']}/host/{rec['id']}"
    return f"{f['ports']}/{rec['port']}/examples/main"


def source_path(rec: dict, root: str = ROOT) -> str:
    return f"{example_dir(rec, root)}/{rec['source']}"


def sections(path: str) -> set[str]:
    with open(path, encoding="utf-8") as f:
        return set(SNIPPET_START.findall(f.read()))


def table(records: list[dict], root: str = ROOT) -> str:
    rows = ["| Example | Kind | Parts | What it shows | Built by | Shown in |", "| --- | --- | --- | --- | --- | --- |"]
    for r in records:
        built = f"`KB_BUILD_EXAMPLES`, ctest `example_{r['id']}`" if r["kind"] == "host" else \
            f"`ci-esp32.yml` (`{r['port']}` port, ESP-IDF v5.5)"
        shown = f"`{r['doc']}`" if r.get("doc") else "-"
        rows.append(f"| `{source_path(r, root)}` | {r['kind']} | {', '.join(r['parts'])} | {r['summary']} | {built} | {shown} |")
    return "\n".join(rows)


def catalog_form(records: list[dict], root: str = ROOT) -> list[dict]:
    """What the configurator catalogue carries per example: no documentation-only fields."""
    out = []
    for r in sorted(records, key=lambda x: x["id"]):
        item = {"id": r["id"], "title": r["title"], "kind": r["kind"], "parts": list(r["parts"]),
                "path": source_path(r, root), "summary": r["summary"]}
        if r.get("port"):
            item["port"] = r["port"]
        if r.get("doc"):
            item["doc"] = r["doc"]
        out.append(item)
    return out


def check(root: str = ROOT) -> list[str]:
    problems: list[str] = []
    try:
        records = load(root)
    except (OSError, ValueError) as e:
        return [str(e)]
    f = folders(root)
    ids = [r.get("id") for r in records]
    for i in sorted({x for x in ids if ids.count(x) > 1}):
        problems.append(f"duplicate example id {i}")
    for r in records:
        rid = r.get("id", "?")
        for key in ("id", "title", "kind", "parts", "source", "summary"):
            if key not in r:
                problems.append(f"{rid}: missing field {key}")
        unknown = set(r) - FIELDS
        if unknown:
            problems.append(f"{rid}: unknown field(s) {', '.join(sorted(unknown))}")
        if r.get("kind") not in KINDS:
            problems.append(f"{rid}: kind must be one of {', '.join(KINDS)}")
            continue
        if "source" not in r or "id" not in r or "parts" not in r:
            continue
        if r["kind"] == "esp-idf" and not r.get("port"):
            problems.append(f"{rid}: an esp-idf example names its port")
            continue
        src = os.path.join(root, source_path(r, root))
        if not os.path.isfile(src):
            problems.append(f"{rid}: source {source_path(r, root)} does not exist")
            continue
        if r["kind"] == "host":
            for need in ("CMakeLists.txt", "README.md"):
                if not os.path.isfile(os.path.join(root, example_dir(r, root), need)):
                    problems.append(f"{rid}: {example_dir(r, root)}/{need} does not exist")
        have = sections(src)
        snippets = r.get("snippets") or []
        for s in snippets:
            if s not in have:
                problems.append(f"{rid}: {source_path(r, root)} has no snippet section '{s}'")
        doc = r.get("doc")
        if snippets and not doc:
            problems.append(f"{rid}: snippets listed but no doc page")
        if doc:
            doc_file = os.path.join(root, doc)
            if not os.path.isfile(doc_file):
                problems.append(f"{rid}: doc page {doc} does not exist")
            else:
                with open(doc_file, encoding="utf-8") as fh:
                    included = {(m.group(1), m.group(2)) for m in SNIPPET_INCLUDE.finditer(fh.read())}
                for s in snippets:
                    if (source_path(r, root), s) not in included:
                        problems.append(f"{rid}: {doc} does not include {source_path(r, root)}:{s}")
                if not snippets:
                    problems.append(f"{rid}: doc page {doc} named but no snippet sections")

    # Folders against the index.
    host_dir = os.path.join(root, f["examples"], "host")
    on_disk = sorted(d for d in os.listdir(host_dir) if os.path.isdir(os.path.join(host_dir, d))) if os.path.isdir(host_dir) else []
    listed = sorted(r["id"] for r in records if r.get("kind") == "host" and "id" in r)
    for d in on_disk:
        if d not in listed:
            problems.append(f"{f['examples']}/host/{d} is not in examples.yml")
    for d in listed:
        if d not in on_disk:
            problems.append(f"host example {d} has no folder")

    # ESP-IDF apps against each port's app matrix.
    ports_dir = os.path.join(root, f["ports"])
    for port in sorted(os.listdir(ports_dir)) if os.path.isdir(ports_dir) else []:
        cfg = os.path.join(ports_dir, port, "examples", "app_config.yml")
        if not os.path.isfile(cfg):
            continue
        with open(cfg, encoding="utf-8") as fh:
            text = fh.read()
        apps = re.findall(r"^  ([a-z0-9_]+):\n    description:", text, flags=re.M)
        mine = sorted(r["id"] for r in records if r.get("kind") == "esp-idf" and r.get("port") == port)
        for a in apps:
            if a not in mine:
                problems.append(f"app {a} of the {port} port is not in examples.yml")
        for a in mine:
            if a not in apps:
                problems.append(f"esp-idf example {a} is not an app of the {port} port's app_config.yml")

    # Every documentation snippet taken from the examples folder resolves to a file and a section.
    docs_dir = os.path.join(root, f["docs"])
    ex_prefix = f["examples"] + "/"
    for dirpath, _, files in os.walk(docs_dir):
        for name in files:
            if not name.endswith(".md"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as fh:
                for m in SNIPPET_INCLUDE.finditer(fh.read()):
                    target, section = m.group(1), m.group(2)
                    if target.startswith(ex_prefix):
                        tp = os.path.join(root, target)
                        rel = os.path.relpath(path, root)
                        if not os.path.isfile(tp):
                            problems.append(f"{rel}: snippet source {target} does not exist")
                        elif section and section not in sections(tp):
                            problems.append(f"{rel}: {target} has no snippet section '{section}'")

    # README table.
    readme = os.path.join(root, f["examples"], "README.md")
    try:
        with open(readme, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        problems.append(f"{f['examples']}/README.md does not exist")
        text = ""
    if text:
        m = re.search(re.escape(TABLE_BEGIN) + r"\n(.*?)\n" + re.escape(TABLE_END), text, flags=re.S)
        if not m:
            problems.append(f"{f['examples']}/README.md has no {TABLE_BEGIN} ... {TABLE_END} region")
        elif m.group(1).strip() != table(records, root).strip():
            problems.append(f"{f['examples']}/README.md table is stale (run tools/examples/examples.py write)")

    # Configurator catalogue.
    cat = os.path.join(root, "configurator", "data", "catalog.json")
    if os.path.isfile(cat):
        with open(cat, encoding="utf-8") as fh:
            have = json.load(fh).get("examples")
        if have != catalog_form(records, root):
            problems.append("configurator/data/catalog.json 'examples' differs from the index "
                            "(configurator/tools/build_catalog.py)")
    return problems


def write(root: str = ROOT) -> bool:
    records = load(root)
    readme = os.path.join(root, folders(root)["examples"], "README.md")
    with open(readme, encoding="utf-8") as fh:
        text = fh.read()
    new = re.sub(re.escape(TABLE_BEGIN) + r"\n.*?\n" + re.escape(TABLE_END),
                 lambda _: TABLE_BEGIN + "\n" + table(records, root) + "\n" + TABLE_END, text, flags=re.S)
    if new != text:
        with open(readme, "w", encoding="utf-8") as fh:
            fh.write(new)
    return new != text


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("cmd", choices=("check", "write", "json"))
    args = ap.parse_args(argv)
    if args.cmd == "json":
        print(json.dumps(catalog_form(load(args.root), args.root), indent=1, sort_keys=True))
        return 0
    if args.cmd == "write":
        print("examples/README.md rewritten" if write(args.root) else "examples/README.md already up to date")
        return 0
    problems = check(args.root)
    for p in problems:
        print(p)
    print(f"examples: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
