#!/usr/bin/env python3
# Runs the kb tool's unit tests (ctest kb_cli_unit). Exit 77, the CTest skip code, when PyYAML is missing on a
# workstation: the YAML input paths cannot be tested then (the resolver itself needs only the standard library). In
# CI (CI set) or with KB_REQUIRE_PYYAML=1 (tools/preflight.sh) a missing PyYAML is a failure, never a skip.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Test runner of the kb tool."""

import os
import sys
import unittest

TOOLS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    try:
        import yaml  # noqa: F401
    except ImportError:
        if os.environ.get("CI") or os.environ.get("KB_REQUIRE_PYYAML") == "1":
            print(f"PyYAML is not installed for {sys.executable}: the kb tool tests cannot run (CI or "
                  "KB_REQUIRE_PYYAML=1 needs them); python3 -m pip install pyyaml")
            return 1
        print("PyYAML is not installed: kb tool tests skipped (python3 -m pip install pyyaml)")
        return 77
    sys.path.insert(0, TOOLS)
    suite = unittest.defaultTestLoader.discover(os.path.join(TOOLS, "kb", "tests"), top_level_dir=TOOLS)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
