# The emitted files (tools/kb/emit.py).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Unit tests of kb.emit."""

import json
import unittest

from kb import emit, resolver

from . import common


class EmitTest(unittest.TestCase):
    def lock(self, **extra):
        img = {"schema": 1, "image": "b", "kb_core": {"version": "0.4.0"}, "mcu": {"family": "esp32"}}
        img.update(extra)
        return resolver.resolve(common.base_catalog(), img)["lock"]

    def test_canonical_json(self):
        self.assertEqual(emit.canonical_json({"b": 1, "a": [1, {"d": None, "c": "é"}]}),
                         '{\n  "a": [\n    1,\n    {\n      "c": "é",\n      "d": null\n    }\n  ],\n  "b": 1\n}\n')

    def test_lock_is_yaml_and_json(self):
        lock = self.lock(drivers=["pcal9555"])
        text = emit.lock_text(lock)
        self.assertTrue(text.startswith("# "))
        self.assertEqual(json.loads(text.split("\n", 1)[1]), lock)
        try:
            import yaml
        except ImportError:  # pragma: no cover
            self.skipTest("PyYAML")
        self.assertEqual(yaml.safe_load(text), lock)

    def test_cmake_order(self):
        text = emit.cmake_text(self.lock(drivers=["ws2812_rmt", "pcal9555"]))
        sets = [line.split()[0] for line in text.splitlines() if line.startswith("set(")]
        self.assertEqual(sets, ["set(KB_MCU", "set(KB_RTOS", "set(KB_PARTS", "set(KB_ENABLE_PCAL9555",
                                "set(KB_ENABLE_WS2812_RMT", "set(KB_DRIVER_PCAL9555_DIR", "set(KB_DRIVER_WS2812_RMT_DIR"])
        self.assertIn('set(KB_PARTS "utils;rtos;interfaces;handlers")', text)
        self.assertIn('set(KB_DRIVER_PCAL9555_DIR "${CMAKE_CURRENT_LIST_DIR}/external/drivers/driver-pcal9555")', text)

    def test_fetch_uses_the_external_layout(self):
        lock = self.lock(drivers=["pcal9555"])
        text = emit.fetch_text(lock)
        commands = [line for line in text.splitlines() if not line.startswith("#")]
        self.assertFalse(any("--recursive" in line for line in commands))
        self.assertIn(f"pin 'external/kb-core' 'https://github.com/kokeb-labs/kb-core.git' 'v0.4.0' "
                      f"'{lock['kb_core']['commit']}'", text)
        self.assertIn(f"pin 'external/drivers/driver-pcal9555' 'https://github.com/kokeb-labs-drivers/"
                      f"driver-pcal9555.git' 'v0.1.0' '{lock['drivers'][0]['commit']}'", text)
        self.assertTrue(text.startswith("#!/bin/sh\n"))
        self.assertEqual([p["path"] for p in emit.layout(lock)], ["external/kb-core", "external/drivers/driver-pcal9555"])

    def test_quote(self):
        self.assertEqual(emit._quote("it's"), "'it'\\''s'")
        self.assertEqual(emit._quote(None), "''")

    def test_presets(self):
        doc = json.loads(emit.presets_text(self.lock(drivers=["pcal9555"])))
        self.assertEqual(doc["version"], 6)
        variables = doc["configurePresets"][0]["cacheVariables"]
        self.assertEqual(variables["KB_RTOS"], "FREERTOS")
        self.assertEqual(variables["KB_DRIVER_PCAL9555_DIR"], "${sourceDir}/external/drivers/driver-pcal9555")

    def test_files_are_deterministic(self):
        self.assertEqual(emit.files(self.lock(drivers=["pcal9555"])), emit.files(self.lock(drivers=["pcal9555"])))
        self.assertEqual(sorted(emit.files(self.lock())),
                         ["CMakePresets.json", "kb-fetch.sh", "kb-image.lock.yml", "kb_image.cmake"])


if __name__ == "__main__":
    unittest.main()
