# Versions and tags of the kb family: vX.Y.Z and vX.Y.Z-rc.N only (ADR-0012). Resolver specification:
# docs/design/assembler.md, "Versions".
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Parse and order kb versions (``X.Y.Z`` or ``X.Y.Z-rc.N``) and their tags (``v`` + version)."""

import re

TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.(0|[1-9][0-9]*))?$")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.(0|[1-9][0-9]*))?$")

# A release sorts after every release candidate of the same X.Y.Z.
_RELEASE_RANK = 1 << 62


def parse_version(text):
    """Return the sort key ``(x, y, z, rc)`` of a version, or None when it is not ``X.Y.Z[-rc.N]``."""
    if not isinstance(text, str):
        return None
    m = VERSION_RE.match(text)
    if m is None:
        return None
    rc = _RELEASE_RANK if m.group(5) is None else int(m.group(5))
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), rc)


def is_tag(text):
    """True when ``text`` is a release or release-candidate tag."""
    return isinstance(text, str) and TAG_RE.match(text) is not None


def tag_kind(tag):
    """``release`` or ``rc`` for a tag; None for anything else."""
    if not is_tag(tag):
        return None
    return "rc" if "-rc." in tag else "release"


def compare(a, b):
    """-1, 0 or 1 comparing two valid versions."""
    ka, kb = parse_version(a), parse_version(b)
    if ka is None or kb is None:
        raise ValueError(f"not a version: {a!r} or {b!r}")
    return (ka > kb) - (ka < kb)
