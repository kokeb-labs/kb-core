#!/usr/bin/env python3
# Runs the examples tests (ctest kb_examples_unit).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
import os
import sys
import unittest

here = os.path.dirname(os.path.abspath(__file__))
suite = unittest.defaultTestLoader.discover(here, pattern="test_*.py")
sys.exit(0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1)
