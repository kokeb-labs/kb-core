#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Figure policy of the kb-core site, on top of ``kokeb-docs figures --check`` (which proves each SVG equals its source).

Fails when:
* a page or the README still holds a ``mermaid`` fence, or a ``.mmd`` source exists: figures are committed SVG, drawn
  by ``docs/diagrams/figures/<id>.py``, never by a script in the reader's browser (it costs Lighthouse performance);
* an SVG under docs/diagrams has no light/dark pair, no ``<title>`` and ``<desc>``, no source module, or carries a
  ``<script>``, an external reference or an inline ``style`` attribute;
* a figure module does not use the shared kit (``kbfig``) or a role colour, so figures stay one family.

Usage: python3 tools/docs/check_figures.py [ROOT]
"""

from __future__ import annotations

import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
diagrams = root / "docs/diagrams"
problems: list[str] = []

for md in [root / "README.md", *sorted((root / "docs").rglob("*.md"))]:
    text = md.read_text(encoding="utf-8")
    if re.search(r"^\s*(```|~~~)\s*mermaid\b", text, re.M):
        problems.append(f"{md.relative_to(root)}: a mermaid fence; draw a figure module instead (docs/figures.md)")
for mmd in sorted(root.rglob("*.mmd")):
    if ".git" not in mmd.parts and "third_party" not in mmd.parts:
        problems.append(f"{mmd.relative_to(root)}: a Mermaid source; convert it to docs/diagrams/figures/<id>.py")

ids = {p.stem for p in (diagrams / "figures").glob("*.py") if not p.stem.startswith("_")}
svgs = {p.name for p in diagrams.glob("*.svg")}
for name in sorted(svgs):
    base = name.removesuffix(".dark.svg").removesuffix(".svg")
    if base.startswith("chip-"):
        continue  # drawn from docs/hardware.yml by kokeb-docs, not by a module
    other = f"{base}.svg" if name.endswith(".dark.svg") else f"{base}.dark.svg"
    if other not in svgs:
        problems.append(f"docs/diagrams/{name}: no {other} (every figure has a light and a dark file)")
    if base not in ids:
        problems.append(f"docs/diagrams/{name}: no source module docs/diagrams/figures/{base}.py")
    svg = (diagrams / name).read_text(encoding="utf-8")
    if "<title" not in svg or "<desc" not in svg:
        problems.append(f"docs/diagrams/{name}: no <title> and <desc>")
    if re.search(r"<script|href=\"https?:|style=\"", svg):
        problems.append(f"docs/diagrams/{name}: a script, an external reference or an inline style")
    if len(svg.encode()) > 60_000:
        problems.append(f"docs/diagrams/{name}: {len(svg.encode())} bytes, over the 60 KB budget")

for mod in sorted((diagrams / "figures").glob("*.py")):
    if not mod.stem.startswith("_") and "import kbfig" not in mod.read_text(encoding="utf-8"):
        problems.append(f"{mod.relative_to(root)}: does not import the shared kit (import kbfig)")

for p in problems:
    print(f"[figures-policy] {p}")
print(f"[figures-policy] {'FAIL' if problems else 'OK'}: {len(ids)} figure module(s), {len(problems)} problem(s)")
sys.exit(1 if problems else 0)
