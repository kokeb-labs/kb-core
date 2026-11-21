# Reading YAML or JSON input. PyYAML is needed only for YAML files; JSON input works with the standard library.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Load a YAML or JSON document, with a clear message when PyYAML is missing."""

import json

try:  # pragma: no cover - depends on the environment
    import yaml
except ImportError:  # pragma: no cover
    yaml = None


class InputError(Exception):
    """A file could not be read or parsed; the message says which and why."""


def have_yaml():
    return yaml is not None


def _json_after_comments(text):
    """The text after leading '#' comment and blank lines, when it is a JSON object (a lock is written so)."""
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines) and (not lines[i].strip() or lines[i].lstrip().startswith("#")):
        i += 1
    rest = "".join(lines[i:])
    return rest if rest.lstrip().startswith("{") else None


def loads(text, name="<input>"):
    stripped = text.lstrip()
    body = _json_after_comments(text)
    if body is not None and yaml is None:
        # kb-image.lock.yml is one comment line and canonical JSON: readable without PyYAML.
        try:
            return json.loads(body)
        except ValueError:
            pass
    if name.endswith(".json") or stripped.startswith("{") and not name.endswith((".yml", ".yaml")):
        try:
            return json.loads(text)
        except ValueError as exc:
            raise InputError(f"{name}: not valid JSON: {exc}") from None
    if yaml is None:
        raise InputError(f"{name}: reading YAML needs PyYAML (python3 -m pip install pyyaml), or pass JSON")
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise InputError(f"{name}: not valid YAML: {exc}") from None


def load(path):
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        raise InputError(f"{path}: {exc.strerror}") from None
    return loads(text, path)
