#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The text of the one GitHub issue the weekly scan keeps open, from a vulnerabilities.json.

  issue_body.py FILE            the issue body on stdout, ending with a marker line that lists the advisory ids
  issue_body.py FILE --ids      only the sorted ids ("component:id"), one per line (what the marker holds)
  issue_body.py --marker TEXT   the marker line found in an existing body, or nothing

The workflow compares the marker of the open issue with the new one: equal, the issue is left alone; different, the
body is replaced and a comment says what changed.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

MARK = "<!-- kb-vulns:"


def ids(doc: dict) -> list[str]:
    return sorted(f"{c['name']}:{a['id']}" for c in doc["components"] for a in c["advisories"])


def marker(doc: dict) -> str:
    return f"{MARK} {','.join(ids(doc))} -->"


def body(doc: dict) -> str:
    t = doc["totals"]
    out = [f"The weekly scan ({doc['scanner']}, {doc['source']}) on **{doc['checked_at']}** found {t['advisories']} "
           f"advisor{'y' if t['advisories'] == 1 else 'ies'} for {t['with_advisories']} of {t['components']} "
           "pinned components. An advisory means the database lists the pinned commit as affected; read it, decide "
           "whether kb-core is exposed, and record the decision (SOUP list, or move the pin).", ""]
    for c in doc["components"]:
        if not c["count"]:
            continue
        out += [f"### {c['name']} {c['version']} (`{c['commit'][:12]}`)", ""]
        out += [f"- [{a['id']}]({a['url']}) ({a['severity']}): {a['summary']}" for a in c["advisories"]]
        out.append("")
    out += ["The full list is in `docs/assurance/vulnerabilities.md` on the published site. This issue is kept up to "
            "date by `.github/workflows/ci-vulnerabilities.yml`; it is not closed automatically.", "", marker(doc), ""]
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--marker":
        m = re.search(re.escape(MARK) + r".*?-->", argv[2])
        if m:
            print(m.group(0))
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    doc = json.loads(pathlib.Path(argv[1]).read_text(encoding="utf-8"))
    if "--ids" in argv[2:]:
        print("\n".join(ids(doc)))
    else:
        sys.stdout.write(body(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
