#!/usr/bin/env python3
# Runs the vulnerability scan tests (ctest kb_vuln_unit). No network: every scan answers from a fixture.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
import os
import sys
import unittest

here = os.path.dirname(os.path.abspath(__file__))
suite = unittest.defaultTestLoader.discover(here, pattern="test_*.py")
sys.exit(0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1)
