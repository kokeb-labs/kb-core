#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The repository layout (tools/layout/paths.yml): lookups for scripts and two repository checks.

  layout.py get <name>          the path of a layout entry, relative to the root (--abs: absolute)
  layout.py list                every layout entry as name=path
  layout.py vendor              the files a consumer that vendors kb-core copies, resolved (one per line)
  layout.py vendor              the files a consumer that vendors kb-core copies, resolved (one per line)
  layout.py root                fail when a tracked top-level entry is not in the allowlist (root_dirs, root_files)
  layout.py stale [--fix]       fail when a tracked text file still names a retired location; --fix rewrites it

paths.yml is flat (`key: value`, `- item`) so that CMake and Node can read it too; this parser needs no PyYAML.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PATHS_YML = os.path.join(HERE, "paths.yml")

# Files that legitimately describe the old layout or are not ours to rewrite.


def load(path: str = PATHS_YML) -> dict:
    """Parse the flat subset: top-level `section:`, `  key: value` maps and `  - item` lists."""
    data: dict = {}
    section = None
    with open(path, encoding="utf-8") as f:
        for number, raw in enumerate(f, 1):
            line = raw.split(" #")[0].rstrip() if not raw.lstrip().startswith("#") else ""
            if not line.strip():
                continue
            m = re.match(r"^([A-Za-z_]+):\s*$", line)
            if m:
                section = m.group(1)
                data[section] = None
                continue
            m = re.match(r"^\s+-\s+(\S+)$", line)
            if m and section:
                if data[section] is None:
                    data[section] = []
                data[section].append(m.group(1).strip("\"'"))
                continue
            m = re.match(r"^\s+(\S+):\s+(\S+)$", line)
            if m and section:
                if data[section] is None:
                    data[section] = {}
                data[section][m.group(1)] = m.group(2).strip("\"'")
                continue
            raise ValueError(f"{path}:{number}: cannot parse {raw.rstrip()!r}")
    return data


def get(name: str, absolute: bool = False, data: dict | None = None, root: str = ROOT) -> str:
    layout = (data or load())["layout"]
    if name not in layout:
        raise KeyError(f"no layout entry {name!r} in paths.yml")
    return os.path.join(root, layout[name]) if absolute else layout[name]


def vendor_paths(data: dict) -> list[str]:
    """The `vendor` list with every `{name}` replaced by that layout entry."""
    out = []
    for item in data["vendor"]:
        out.append(re.sub(r"\{([A-Za-z_]+)\}", lambda m: data["layout"][m.group(1)], item))
    return out


def tracked(root: str = ROOT, untracked: bool = True) -> list[str]:
    """Paths of the checkout: tracked files, plus (untracked=True) new files git does not ignore."""
    cmd = ["git", "-C", root, "ls-files", "-z", "--cached"] + (["--others", "--exclude-standard"] if untracked else [])
    out = subprocess.run(cmd, check=True, capture_output=True).stdout.decode()
    return [p for p in out.split("\0") if p]


def root_entries(root: str = ROOT) -> list[str]:
    """What the root allowlist judges: the git-tracked paths (a folder a CI job or a tool leaves in the root, not
    tracked, is not part of the repository layout). Outside a git checkout: the root's own listing, without .git."""
    try:
        return tracked(root, untracked=False)
    except (subprocess.CalledProcessError, FileNotFoundError):
        found = []
        for name in sorted(os.listdir(root)):
            if name == ".git":
                continue
            found.append(name + "/x" if os.path.isdir(os.path.join(root, name)) else name)
        return found


def submodule_paths(root: str = ROOT) -> set[str]:
    out = subprocess.run(["git", "-C", root, "ls-files", "-s"], check=True, capture_output=True).stdout.decode()
    return {line.split("\t", 1)[1] for line in out.splitlines() if line.startswith("160000")}


def unexpected_root_entries(entries: list[str], data: dict) -> list[str]:
    """Top-level names in `entries` (tracked paths) that are neither allowed directories nor allowed files."""
    dirs, files = set(data["root_dirs"]), set(data["root_files"])
    bad = []
    for top in sorted({e.split("/", 1)[0] for e in entries}):
        is_dir = any(e.startswith(top + "/") for e in entries)
        if is_dir and top not in dirs:
            bad.append(top + "/")
        elif not is_dir and top not in files:
            bad.append(top)
    return bad


def missing_root_entries(root: str, data: dict) -> list[str]:
    return [d + "/" for d in data["root_dirs"] if not os.path.isdir(os.path.join(root, d))]


def stale_patterns(data: dict) -> list[tuple[re.Pattern, str, str]]:
    """(pattern, old, replacement) per retired location whose old path is no longer a layout entry's value."""
    layout, current = data["layout"], set(data["layout"].values())
    out = []
    for old, name in data["retired"].items():
        if old in current:  # the old location is in use again
            continue
        # A path start (not inside another path, URL, option or word) or the repository root in a vendored tree
        # (external/kb-core/) or a GitHub blob/tree URL, then the old prefix, then a separator.
        start = r"(?:(?<![\w./@+=~-])|(?<=external/kb-core/)|(?<=/blob/main/)|(?<=/tree/main/))"
        out.append((re.compile(start + re.escape(old) + r"(?=/)"), old, layout[name]))
    return out


def stale_references(root: str, data: dict, fix: bool = False) -> list[str]:
    patterns = stale_patterns(data)
    skip = tuple(data.get("stale_skip") or [])
    submodules = submodule_paths(root)
    found = []
    for rel in tracked(root):
        if rel in skip or any(rel.startswith(s + "/") for s in skip) or rel in submodules:
            continue
        if any(rel == s or rel.startswith(s + "/") for s in submodules):
            continue
        path = os.path.join(root, rel)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except (UnicodeDecodeError, OSError):
            continue
        new = text
        for pattern, old, repl in patterns:
            for m in pattern.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                found.append(f"{rel}:{line}: retired location '{old}/' (now '{repl}/')")
            new = pattern.sub(repl, new)
        if fix and new != text:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
    return found


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=ROOT)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("get")
    g.add_argument("name")
    g.add_argument("--abs", action="store_true")
    sub.add_parser("list")
    sub.add_parser("vendor")
    sub.add_parser("root")
    s = sub.add_parser("stale")
    s.add_argument("--fix", action="store_true")
    args = ap.parse_args(argv)
    data = load(os.path.join(args.root, "tools", "layout", "paths.yml"))
    if args.cmd == "get":
        try:
            print(get(args.name, args.abs, data, args.root))
        except KeyError as e:
            print(e.args[0], file=sys.stderr)
            return 2
        return 0
    if args.cmd == "list":
        for k, v in data["layout"].items():
            print(f"{k}={v}")
        return 0
    if args.cmd == "vendor":
        print("\n".join(vendor_paths(data)))
        return 0
    if args.cmd == "root":
        bad = unexpected_root_entries(root_entries(args.root), data)
        gone = missing_root_entries(args.root, data)
        for b in bad:
            print(f"unexpected top-level entry: {b} (add it to root_dirs/root_files in tools/layout/paths.yml, or move it)")
        for d in gone:
            print(f"allowed top-level folder does not exist: {d}")
        for name, rel in data["layout"].items():
            if not os.path.isdir(os.path.join(args.root, rel)):
                print(f"layout entry {name}: {rel} is not a folder")
                bad.append(rel)
        print(f"root: {len(bad) + len(gone)} problem(s)")
        return 1 if bad or gone else 0
    found = stale_references(args.root, data, fix=args.fix)
    for f in found:
        print(f)
    print(f"stale: {len(found)} reference(s)" + (" rewritten" if args.fix and found else ""))
    return 1 if found and not args.fix else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
