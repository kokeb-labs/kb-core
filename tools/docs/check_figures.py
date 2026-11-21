#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Figure policy of the kb-core site, on top of ``kokeb-docs figures --check`` (which proves each SVG equals its source).

Fails when:
* a page or the README still holds a ``mermaid`` fence, or a ``.mmd`` source exists: figures are committed SVG, drawn
  from ``docs/model/<id>.<kind>.yml`` (or a module in ``docs/diagrams/figures/``), never by a script in the reader's
  browser (it costs Lighthouse performance);
* an SVG under docs/diagrams has no light/dark pair, no ``<title>`` and ``<desc>``, no source, or carries a
  ``<script>``, an external reference or an inline ``style`` attribute;
* a figure does not come from the shared kit: each figure is a model file of one of the figkit kinds (state, graph,
  seq, timeline, matrix, bytes, pipeline) or a module that draws with figkit (``kokeb_mkdocs_theme.figkit``, its kinds
  preferred), so figures stay one family; a module may not import a private helper layer.

Usage: python3 tools/docs/check_figures.py [ROOT]
"""

from __future__ import annotations

import pathlib
import re
import sys

KINDS = ("state", "graph", "seq", "timeline", "matrix", "bytes", "pipeline")

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
diagrams = root / "docs/diagrams"
problems: list[str] = []

for md in [root / "README.md", *sorted((root / "docs").rglob("*.md"))]:
    text = md.read_text(encoding="utf-8")
    if re.search(r"^\s*(```|~~~)\s*mermaid\b", text, re.M):
        problems.append(f"{md.relative_to(root)}: a mermaid fence; draw a figure model instead (docs/figures.md)")
for mmd in sorted(root.rglob("*.mmd")):
    if ".git" not in mmd.parts and "third_party" not in mmd.parts:
        problems.append(f"{mmd.relative_to(root)}: a Mermaid source; convert it to docs/model/<id>.<kind>.yml")

modules = {p.stem: p for p in (diagrams / "figures").glob("*.py") if not p.stem.startswith("_")}
models: dict[str, pathlib.Path] = {}
for p in sorted((root / "docs/model").glob("*.yml")):
    stem, _, kind = p.name.removesuffix(".yml").rpartition(".")
    if stem and kind in KINDS:
        models[stem] = p
for fid in sorted(set(modules) & set(models)):
    problems.append(f"docs/model/{models[fid].name}: figure {fid!r} also has a module; keep one source")
ids = set(modules) | set(models)

svgs = {p.name for p in diagrams.glob("*.svg")}
for name in sorted(svgs):
    svg = (diagrams / name).read_text(encoding="utf-8")
    if "<title" not in svg or "<desc" not in svg:
        problems.append(f"docs/diagrams/{name}: no <title> and <desc>")
    if re.search(r"<script|href=\"https?:|style=\"", svg):
        problems.append(f"docs/diagrams/{name}: a script, an external reference or an inline style")
    if len(svg.encode()) > 60_000:
        problems.append(f"docs/diagrams/{name}: {len(svg.encode())} bytes, over the 60 KB budget")
    if name.endswith(".adaptive.svg"):
        base = name.removesuffix(".adaptive.svg")
        if f"{base}.svg" not in svgs:
            problems.append(f"docs/diagrams/{name}: no {base}.svg beside it")
        continue  # the one-file form of a figure that also has its light and dark pair
    base = name.removesuffix(".dark.svg").removesuffix(".svg")
    if base.startswith("chip-"):
        continue  # drawn from docs/hardware.yml by kokeb-docs, not by a model or a module
    other = f"{base}.svg" if name.endswith(".dark.svg") else f"{base}.dark.svg"
    if other not in svgs:
        problems.append(f"docs/diagrams/{name}: no {other} (every figure has a light and a dark file)")
    if base not in ids:
        problems.append(f"docs/diagrams/{name}: no source docs/model/{base}.<kind>.yml or "
                        f"docs/diagrams/figures/{base}.py")

for fid, mod in sorted(modules.items()):
    text = mod.read_text(encoding="utf-8")
    if not re.search(r"^\s*from kokeb_mkdocs_theme\.figkit(\.kinds)? import ", text, re.M):
        problems.append(f"{mod.relative_to(root)}: does not draw with figkit (from kokeb_mkdocs_theme.figkit[.kinds] "
                        "import ...); prefer a model file docs/model/<id>.<kind>.yml")
    if re.search(r"^\s*(import|from)\s+kbfig\b", text, re.M):
        problems.append(f"{mod.relative_to(root)}: imports kbfig, which the figkit kinds replaced")

for p in problems:
    print(f"[figures-policy] {p}")
print(f"[figures-policy] {'FAIL' if problems else 'OK'}: {len(models)} model file(s), {len(modules)} module(s), "
      f"{len(problems)} problem(s)")
sys.exit(1 if problems else 0)
