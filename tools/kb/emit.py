# The files `kb resolve` writes from a lock: the lock itself, a CMakePresets.json fragment, kb_image.cmake and the
# fetch script. Byte-for-byte the same as configurator/resolver.mjs emits (tests/resolver/vectors, ADR-0014).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Deterministic text of every file a resolved image produces (pure, no I/O)."""

import json

LOCK_FILE = "kb-image.lock.yml"
PRESETS_FILE = "CMakePresets.json"
CMAKE_FILE = "kb_image.cmake"
FETCH_FILE = "kb-fetch.sh"


def canonical_json(value):
    """Sorted keys, two-space indent, UTF-8 as is, one final newline: what JSON.stringify gives on sorted keys."""
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def lock_text(lock):
    # JSON is YAML: the lock is read by any YAML or JSON parser and is byte-identical from both front ends.
    return "# kb image lock (schema 1), written by kb resolve: do not edit.\n" + canonical_json(lock)


def _preset_variables(lock):
    out = dict(lock["cmake"])
    for drv in lock["drivers"]:
        out[f"KB_DRIVER_{drv['name'].upper()}_DIR"] = "${sourceDir}/external/drivers/" + drv["folder"]
    return out


def presets_text(lock):
    name = lock["image"]
    return canonical_json({
        "version": 6,
        "configurePresets": [{
            "name": name,
            "displayName": f"kb image {name}",
            "binaryDir": "${sourceDir}/build/" + name,
            "cacheVariables": _preset_variables(lock),
        }],
    })


def _cmake_order(key):
    fixed = ("KB_MCU", "KB_RTOS", "KB_PARTS")
    return (fixed.index(key), "") if key in fixed else (len(fixed), key)


def cmake_text(lock):
    kb = lock["kb_core"]
    lines = [
        f"# kb image {lock['image']}: written by kb resolve from kb-image.yml (schema 1); do not edit.",
        f"# kb-core {kb['version']} at {kb['commit']}, MCU {lock['mcu']['family']}/{lock['mcu']['target']}, "
        f"RTOS {lock['rtos']}.",
    ]
    for key in sorted(lock["cmake"], key=_cmake_order):
        value = lock["cmake"][key]
        lines.append(f"set({key} {value})" if value == "ON" else f'set({key} "{value}")')
    # The drivers are where kb-fetch.sh puts them: external/drivers/, beside kb-core, not inside it.
    for drv in lock["drivers"]:
        lines.append(f'set(KB_DRIVER_{drv["name"].upper()}_DIR "${{CMAKE_CURRENT_LIST_DIR}}/external/drivers/{drv["folder"]}")')
    return "\n".join(lines) + "\n"


def _quote(text):
    """POSIX shell single-quoted word."""
    return "'" + ("" if text is None else str(text)).replace("'", "'\\''") + "'"


def layout(lock):
    """Where each part goes in the user's repository: submodules under external/ (one layout, ADR-0014)."""
    kb = lock["kb_core"]
    out = [{"label": f"kb-core {kb['version']}", "path": "external/kb-core",
            "url": f"https://github.com/{kb['repo']}.git", "tag": kb["tag"], "commit": kb["commit"]}]
    for port in lock["ports"]:
        if not port["in_tree"]:
            out.append({"label": f"port-{port['name']} {port['version']}", "path": f"external/port-{port['name']}",
                        "url": f"https://github.com/{port['repo']}.git", "tag": port["tag"], "commit": port["commit"]})
    for drv in lock["drivers"]:
        out.append({"label": f"driver {drv['name']} {drv['version']}", "path": f"external/drivers/{drv['folder']}",
                    "url": f"https://github.com/{drv['repo']}.git", "tag": drv["tag"], "commit": drv["commit"]})
    return out


FETCH_HEAD = [
    "#!/bin/sh",
    None,  # the image line
    "# submodules under external/, each checked out at its tag (a kb-core development revision: at its commit) and",
    "# verified against the commit in kb-image.lock.yml (ADR-0012, ADR-0014). A tag that moved is refused. No branch",
    "# is recorded in .gitmodules and nothing is fetched recursively. Rerunning it is safe. Written by kb resolve.",
    "#",
    "#   sh kb-fetch.sh            add or update every pin",
    "#   sh kb-fetch.sh --verify   only check that every submodule is at its pinned commit",
    "set -eu",
    "",
    "mode=add",
    'if [ "${1:-}" = "--verify" ]; then mode=verify; fi',
    'top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "kb-fetch: run this inside a git repository" >&2; exit 1; }',
    'cd "$top"',
    "",
    "die() {",
    "  printf 'kb-fetch: error: %s\\n' \"$*\" >&2",
    "  exit 1",
    "}",
    "",
    "# pin <path> <url> <tag, empty for a commit pin> <commit>",
    "pin() {",
    "  path=$1 url=$2 tag=$3 want=$4",
    '  if [ "$mode" = verify ]; then',
    '    [ -e "$path/.git" ] || die "$path is missing; run without --verify"',
    "  else",
    '    if [ ! -e "$path/.git" ]; then',
    '      git submodule add --quiet "$url" "$path" || die "cannot add $url"',
    "    fi",
    '    if [ -n "$tag" ]; then',
    '      git -C "$path" fetch --quiet origin "+refs/tags/$tag:refs/tags/$tag" || die "cannot fetch $tag of $url"',
    '      got=$(git -C "$path" rev-parse "refs/tags/$tag^{commit}")',
    '      [ "$got" = "$want" ] || die "$path: tag $tag points at $got, expected $want (the tag moved; it is not used)"',
    "    else",
    '      git -C "$path" fetch --quiet origin "$want" || die "cannot fetch commit $want of $url"',
    "    fi",
    '    git -C "$path" -c advice.detachedHead=false checkout --quiet "$want"',
    '    git config -f .gitmodules --unset "submodule.$path.branch" 2>/dev/null || true',
    '    git add .gitmodules "$path"',
    "  fi",
    '  head=$(git -C "$path" rev-parse HEAD)',
    '  [ "$head" = "$want" ] || die "$path is at $head, expected $want"',
    "  printf 'kb-fetch: %s at %s (%s)\\n' \"$path\" \"${tag:-commit}\" \"$want\"",
    "}",
    "",
]


def fetch_text(lock):
    lines = list(FETCH_HEAD)
    lines[1] = f"# kb-fetch.sh: adds kb-core, the MCU port and the drivers of the image \"{lock['image']}\" to this git repository as"
    for part in layout(lock):
        note = "" if part["tag"] else " (development revision, no tag yet)"
        lines.append(f"# {part['label']}{note}")
        lines.append(f"pin {_quote(part['path'])} {_quote(part['url'])} {_quote(part['tag'])} {_quote(part['commit'])}")
    lines += ["", 'if [ "$mode" = add ]; then', '  echo "kb-fetch: done; review with git status and commit the submodules"',
              "fi"]
    return "\n".join(lines) + "\n"


def files(lock):
    """Every file of a resolved image, name to text."""
    return {
        CMAKE_FILE: cmake_text(lock),
        FETCH_FILE: fetch_text(lock),
        LOCK_FILE: lock_text(lock),
        PRESETS_FILE: presets_text(lock),
    }
