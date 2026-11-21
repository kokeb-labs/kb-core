#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Write or check the Subresource Integrity hashes in configurator/index.html.

Every <script src>, <link rel="stylesheet"> and <link rel="modulepreload"> in the page carries integrity="sha384-..." of the file it loads;
run with --write after changing a script, the stylesheet, the vendored library or the catalogue, and CI runs
--check. Usage: sri.py [--write | --check]
"""

from __future__ import annotations

import base64
import hashlib
import re
import sys
from pathlib import Path

CONF = Path(__file__).resolve().parent.parent
PAGE = CONF / "index.html"
TAG = re.compile(r'<(script|link)\b([^>]*?)\b(src|href)="([^"]+)"([^>]*?)integrity="([^"]*)"')


def digest(path: Path) -> str:
    return "sha384-" + base64.b64encode(hashlib.sha384(path.read_bytes()).digest()).decode()


def main(argv: list[str]) -> int:
    write = "--write" in argv
    html = PAGE.read_text()
    bad: list[str] = []

    def fix(m: re.Match) -> str:
        ref = m.group(4)
        target = CONF / ref
        if not target.is_file() or ".." in Path(ref).parts:
            bad.append(f"{ref}: no such file inside configurator/")
            return m.group(0)
        want = digest(target)
        if m.group(6) != want:
            bad.append(f"{ref}: integrity is {m.group(6) or 'empty'}, file is {want}")
        return m.group(0)[: m.start(6) - m.start(0)] + want + m.group(0)[m.end(6) - m.start(0):]

    new = TAG.sub(fix, html)
    tags = len(re.findall(r"<script\b[^>]*\bsrc=|<link\b[^>]*rel=\"(?:stylesheet|modulepreload)\"", html))
    found = len(TAG.findall(html))
    if found != tags:
        print(f"sri: {tags - found} script or stylesheet tag(s) without integrity", file=sys.stderr)
        return 1
    if write:
        PAGE.write_text(new)
        print(f"sri: {found} hashes written")
        return 0
    if bad:
        print("sri: out of date (run tools/sri.py --write):\n  " + "\n  ".join(bad), file=sys.stderr)
        return 1
    print(f"sri: {found} hashes match")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
