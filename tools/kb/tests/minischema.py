# The subset of JSON Schema 2020-12 that catalog/schemas/*.schema.json use, so the tests can check documents against them
# with the standard library only: type, const, enum, pattern, required, properties, additionalProperties, items,
# minItems, anyOf, propertyNames and local $ref.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""A small JSON Schema validator for the kb schemas (tests only)."""

import re

_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "boolean": lambda v: isinstance(v, bool),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "null": lambda v: v is None,
}
KNOWN = {"$schema", "$id", "$defs", "title", "description", "default", "examples", "type", "const", "enum",
         "pattern", "required", "properties", "additionalProperties", "items", "minItems", "anyOf", "propertyNames",
         "$ref"}


def errors(schema, value, root=None, path="$"):
    """Every violation of ``schema`` by ``value``, as 'path: reason' strings."""
    root = root if root is not None else schema
    unknown = set(schema) - KNOWN
    if unknown:
        raise ValueError(f"unsupported keyword(s) {sorted(unknown)} at {path}")
    out = []
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/$defs/"):
            raise ValueError(f"unsupported $ref {ref}")
        out += errors(root["$defs"][ref[len("#/$defs/"):]], value, root, path)
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_TYPES[t](value) for t in types):
            return out + [f"{path}: not {'/'.join(types)}"]
    if "const" in schema and (value != schema["const"] or type(value) is not type(schema["const"])):
        out.append(f"{path}: not {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        out.append(f"{path}: {value!r} not in {schema['enum']}")
    if "pattern" in schema and isinstance(value, str) and not re.search(schema["pattern"], value):
        out.append(f"{path}: {value!r} does not match {schema['pattern']}")
    if "anyOf" in schema and not any(not errors(s, value, root, path) for s in schema["anyOf"]):
        out.append(f"{path}: matches none of anyOf")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                out.append(f"{path}: missing {key}")
        props = schema.get("properties", {})
        for key, item in value.items():
            if "propertyNames" in schema:
                out += errors(schema["propertyNames"], key, root, f"{path}.{key}(name)")
            if key in props:
                out += errors(props[key], item, root, f"{path}.{key}")
            elif schema.get("additionalProperties") is False:
                out.append(f"{path}: unexpected {key}")
            elif isinstance(schema.get("additionalProperties"), dict):
                out += errors(schema["additionalProperties"], item, root, f"{path}.{key}")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            out.append(f"{path}: fewer than {schema['minItems']} items")
        if "items" in schema:
            for i, item in enumerate(value):
                out += errors(schema["items"], item, root, f"{path}[{i}]")
    return out
