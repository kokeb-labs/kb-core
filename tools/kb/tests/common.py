# Shared paths and loaders of the kb tests.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Paths of the repository and the shared resolver vectors."""

import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from kb import repolayout  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
VECTORS = os.path.join(ROOT, "tests", "resolver", "vectors")
SCHEMAS = os.path.join(ROOT, repolayout.path("schemas"))


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def vectors():
    """(file name, vector, catalogue) for every vector, in name order."""
    out = []
    for name in sorted(n for n in os.listdir(VECTORS) if n.endswith(".json")):
        vec = load_json(os.path.join(VECTORS, name))
        cat = vec["catalog"]
        if isinstance(cat, str):
            cat = load_json(os.path.join(VECTORS, cat))
        out.append((name, vec, cat))
    return out


def schema(name):
    return load_json(os.path.join(SCHEMAS, name))


def base_catalog():
    return load_json(os.path.join(VECTORS, "catalogs", "base.json"))
