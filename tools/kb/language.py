# The language floor of a part (docs/language-floor.md): the `language:` block of a driver's driver.yml or of
# kb-core's kb.yml, normalised to what the catalogue records per version. Used by `kb catalog build`
# (tools/kb/catalog.py) and the configurator's catalogue builder (configurator/tools/build_catalog.py), so both
# record the same fields from the same block.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Read and normalise a ``language:`` block."""

import re

#: The registry default (ci-tools tools/standards/standards.yml, profile KB-CPP20), recorded for a version whose file
#: at the tag has no block. The compiler floors are the profile's.
DEFAULT = {"standard": 20, "profile": "KB-CPP20",
           "compilers": {"arm_gcc": "10", "clang": "13", "esp_idf": "5.0", "gcc": "10", "msvc": "19.30"},
           "tested": [20]}
STANDARDS = (17, 20, 23, 26)
COMPILERS = ("arm_gcc", "clang", "esp_idf", "gcc", "msvc")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){0,2}$")
PROFILE_RE = re.compile(r"^KB-CPP[0-9]{2}$")


def default():
    """The registry default as a catalogue entry (a fresh copy)."""
    return {"standard": DEFAULT["standard"], "profile": DEFAULT["profile"], "compilers": dict(DEFAULT["compilers"]),
            "tested": list(DEFAULT["tested"]), "source": "default"}


def normalise(block, source, where):
    """(entry, problems): the catalogue form of a parsed ``language:`` mapping, or the default when ``block`` is None.

    ``source`` names the file (``driver.yml`` or ``kb.yml``); ``where`` prefixes each problem.
    """
    if block is None:
        return default(), []
    problems = []
    if not isinstance(block, dict):
        return None, [f"{where}: language must be a mapping"]
    std = block.get("standard")
    if isinstance(std, bool) or not isinstance(std, int) or std not in STANDARDS:
        problems.append(f"{where}: language.standard must be one of {', '.join(str(s) for s in STANDARDS)}")
    profile = block.get("profile")
    if not (isinstance(profile, str) and PROFILE_RE.match(profile)):
        problems.append(f"{where}: language.profile must be KB-CPPnn")
    comps = block.get("compilers", {})
    if not isinstance(comps, dict):
        problems.append(f"{where}: language.compilers must be a mapping")
        comps = {}
    for key in sorted(comps):
        if key not in COMPILERS:
            problems.append(f"{where}: language.compilers.{key} is not one of {', '.join(COMPILERS)}")
        elif not (isinstance(comps[key], str) and VERSION_RE.match(comps[key])):
            problems.append(f"{where}: language.compilers.{key} must be a quoted version such as \"10\"")
    tested = block.get("tested", [std])
    if not isinstance(tested, list) or any(isinstance(t, bool) or t not in STANDARDS for t in tested):
        problems.append(f"{where}: language.tested must be a list of standards")
        tested = []
    if problems:
        return None, problems
    return {"standard": std, "profile": profile, "compilers": {k: comps[k] for k in sorted(comps)},
            "tested": sorted(set(tested)), "source": source}, []


_KEY_RE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*):\s*(.*?)\s*$")


def _scalar(text):
    text = re.sub(r"(^|\s+)#.*$", "", text).strip()
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        return [_scalar(p) for p in inner.split(",")] if inner else []
    if text.startswith("{") and text.endswith("}"):
        out = {}
        for pair in filter(None, (p.strip() for p in text[1:-1].split(","))):
            k, _, v = pair.partition(":")
            out[k.strip()] = _scalar(v)
        return out
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if re.fullmatch(r"-?[0-9]+", text):
        return int(text)
    return text


def block_from_text(text):
    """The ``language:`` mapping of a YAML document, read without a YAML library (the configurator's builder has
    none): the block's own keys at one indent, ``compilers`` either a nested block or a flow mapping, ``tested`` a
    flow list. None when the document has no top-level ``language:`` key."""
    lines = text.splitlines()
    for i, raw in enumerate(lines):
        if re.match(r"^language:\s*(#.*)?$", raw):
            break
    else:
        return None
    block, parent, indent = {}, None, None
    for raw in lines[i + 1:]:
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        m = _KEY_RE.match(raw)
        if not m or not m.group(1):
            break  # the next top-level key ends the block
        lead, key, value = len(m.group(1)), m.group(2), re.sub(r"(^|\s+)#.*$", "", m.group(3)).strip()
        if indent is None:
            indent = lead
        if lead == indent:
            parent = None
            if value == "":
                block[key] = {}
                parent = key
            else:
                block[key] = _scalar(value)
        elif lead > indent and parent is not None:
            block[parent][key] = _scalar(value)
        else:
            break
    return block
