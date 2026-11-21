#!/usr/bin/env python3
# `python3 tools/kb/kb.py <command>`: runs the kb package from any working directory.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Script entry point of the kb tool."""

import os
import sys

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    # The package is this folder; import it from its parent, never this script as a module named kb.
    sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != here]
    sys.path.insert(0, os.path.dirname(here))
    from kb.cli import main  # noqa: E402

    sys.exit(main())
