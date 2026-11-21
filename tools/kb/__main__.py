# `python3 -m kb` from tools/.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Entry point of ``python3 -m kb``."""

import sys

from .cli import main

sys.exit(main())
