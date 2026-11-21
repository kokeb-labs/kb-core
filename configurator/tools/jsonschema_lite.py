# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""A small JSON Schema (draft 2020-12 subset) validator, standard library only.

Supported keywords: type (string or list), enum, const, pattern, minLength, minimum, maximum, required,
properties, additionalProperties (bool or schema), patternProperties, items, minItems, uniqueItems,
$ref to "#/$defs/<name>", anyOf. Enough for the configurator schemas; anything else in a schema is an error,
so a schema cannot silently ask for a check this validator does not make.
"""

from __future__ import annotations

import re
from typing import Any

_KNOWN = {
    "$schema", "$id", "$defs", "$comment", "title", "description", "type", "enum", "const", "pattern",
    "minLength", "minimum", "maximum", "required", "properties", "additionalProperties", "patternProperties",
    "items", "minItems", "uniqueItems", "$ref", "anyOf", "default", "examples",
}

_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


def validate(instance: Any, schema: dict, root: dict | None = None, path: str = "$") -> list[str]:
    """Return a list of error strings; empty when the instance conforms."""
    root = root if root is not None else schema
    errors: list[str] = []
    unknown = set(schema) - _KNOWN
    if unknown:
        return [f"{path}: schema uses unsupported keywords {sorted(unknown)}"]
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/$defs/"):
            return [f"{path}: unsupported $ref {ref}"]
        return validate(instance, root["$defs"][ref[len("#/$defs/"):]], root, path)
    if "anyOf" in schema:
        if not any(not validate(instance, sub, root, path) for sub in schema["anyOf"]):
            errors.append(f"{path}: matches none of anyOf")
        return errors
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_TYPES[t](instance) for t in types):
            return [f"{path}: expected {'|'.join(types)}, got {type(instance).__name__}"]
    if "const" in schema and instance != schema["const"]:
        errors.append(f"{path}: must be {schema['const']!r}")
    if "enum" in schema and instance not in schema["enum"]:
        errors.append(f"{path}: {instance!r} not in {schema['enum']}")
    if isinstance(instance, str):
        if "pattern" in schema and not re.search(schema["pattern"], instance):
            errors.append(f"{path}: {instance!r} does not match {schema['pattern']}")
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errors.append(f"{path}: shorter than {schema['minLength']}")
    if _TYPES["number"](instance):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append(f"{path}: below {schema['minimum']}")
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append(f"{path}: above {schema['maximum']}")
    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                errors.append(f"{path}: missing {key}")
        props = schema.get("properties", {})
        pats = schema.get("patternProperties", {})
        extra = schema.get("additionalProperties", True)
        for key, value in instance.items():
            sub = f"{path}.{key}"
            if key in props:
                errors += validate(value, props[key], root, sub)
                continue
            matched = [p for p in pats if re.search(p, key)]
            for p in matched:
                errors += validate(value, pats[p], root, sub)
            if matched:
                continue
            if extra is False:
                errors.append(f"{path}: unexpected key {key}")
            elif isinstance(extra, dict):
                errors += validate(value, extra, root, sub)
    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append(f"{path}: fewer than {schema['minItems']} items")
        if schema.get("uniqueItems"):
            seen = [repr(v) for v in instance]
            if len(seen) != len(set(seen)):
                errors.append(f"{path}: items are not unique")
        if "items" in schema:
            for i, value in enumerate(instance):
                errors += validate(value, schema["items"], root, f"{path}[{i}]")
    return errors
