# The platform table of the catalogue agrees with cmake/kb_build_settings.cmake.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.platforms."""

import os
import re
import unittest

from kb import platforms

from . import common


def cmake_list(text, var):
    m = re.search(rf"set\({var} ([^)]*)\)", text)
    return m.group(1).lower().split() if m else []


class PlatformsTest(unittest.TestCase):
    def test_agrees_with_the_build_settings(self):
        with open(os.path.join(common.ROOT, "cmake", "kb_build_settings.cmake"), encoding="utf-8") as fh:
            text = fh.read()
        mcus = {m["family"]: m for m in platforms.mcus()}
        rtos = {r["name"]: r["status"] for r in platforms.RTOS}
        for fam in cmake_list(text, "_KB_SUPPORTED_MCUS"):
            self.assertEqual(mcus[fam]["status"], "supported", fam)
            self.assertIsNotNone(mcus[fam]["port"], fam)
        for fam in cmake_list(text, "_KB_PLANNED_MCUS"):
            self.assertEqual(mcus[fam]["status"], "planned", fam)
        for name in cmake_list(text, "_KB_SUPPORTED_RTOS"):
            self.assertEqual(rtos[name], "supported", name)
        for name in cmake_list(text, "_KB_PLANNED_RTOS"):
            self.assertEqual(rtos[name], "planned", name)
        self.assertEqual(cmake_list(text, "_KB_ALL_PARTS"), platforms.PARTS)

    def test_table_is_consistent(self):
        rtos = {r["name"] for r in platforms.RTOS}
        for m in platforms.mcus():
            self.assertIn(m["default_rtos"], m["rtos"])
            self.assertTrue(set(m["rtos"]) <= rtos)
            self.assertEqual(m["targets"], sorted(m["targets"]) if m["family"] != "esp32" else m["targets"])
        port = platforms.mcus({"esp32": [{"version": "1.0.0"}]})[0]["port"]
        self.assertEqual(port["versions"], [{"version": "1.0.0"}])
        self.assertEqual(port["repo"], "kokeb-labs-drivers/port-esp32")


if __name__ == "__main__":
    unittest.main()
