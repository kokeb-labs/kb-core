# Where things live in a kb-core checkout: read from the checkout's own tools/layout/paths.yml.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The repository layout for the kb tool (tools/layout/paths.yml is the one place that names it)."""

import importlib.util
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
_LAYOUT_PY = os.path.join(_ROOT, "tools", "layout", "layout.py")

# A kb-core revision from before paths.yml existed kept everything at the top level: `kb catalog build` reads the
# tags of old revisions too, so a checkout without paths.yml is read with this table.
_BEFORE_PATHS_YML = {"drivers": "drivers", "lib": "lib", "ports": "ports", "schemas": "schemas"}


def _module():
    spec = importlib.util.spec_from_file_location("kb_repo_layout", _LAYOUT_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def path(name, root=None):
    """The layout entry `name` (relative to the checkout) of this repository, or of the checkout `root`."""
    mod = _module()
    if root is None:
        return mod.get(name)
    yml = os.path.join(root, "tools", "layout", "paths.yml")
    if os.path.isfile(yml):
        return mod.get(name, data=mod.load(yml))
    return _BEFORE_PATHS_YML[name]
