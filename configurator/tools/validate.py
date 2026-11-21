#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Validate JSON files against a schema with the standard-library validator.

Usage: validate.py SCHEMA FILE [FILE ...]   (a FILE of the form path#pointer validates one member, e.g. #image)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jsonschema_lite import validate  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    schema = json.loads(Path(argv[0]).read_text())
    failed = 0
    for arg in argv[1:]:
        path, _, member = arg.partition("#")
        data = json.loads(Path(path).read_text())
        if member:
            data = data[member]
        errors = validate(data, schema)
        if errors:
            failed += 1
            print(f"{arg}: " + "; ".join(errors[:5]))
    print(f"validate: {len(argv) - 1 - failed} of {len(argv) - 1} valid")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
