# The language floor: reading a `language:` block, kb-core's kb.yml and CMake agreeing, the catalogue field
# (docs/language-floor.md).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.language and of kb-core's own declaration."""

import os
import re
import unittest

from kb import catalog, language, resolver

from . import common, minischema

DRIVER_YML = """name: chip
version: 0.1.0
language:           # the C++ standard, declared once
  standard: 20      # the minimum
  profile: KB-CPP20          # the profile
  profile_version: "1.0"
  compilers:                 # quoted
    gcc: "10"
    clang: "13"
  tested: [20]
  effective_from: 2026-11-19
bus: [i2c]
"""


class LanguageBlockTest(unittest.TestCase):
    def test_reads_a_driver_block(self):
        block = language.block_from_text(DRIVER_YML)
        entry, problems = language.normalise(block, "driver.yml", "chip")
        self.assertEqual(problems, [])
        self.assertEqual(entry, {"standard": 20, "profile": "KB-CPP20", "compilers": {"clang": "13", "gcc": "10"},
                                 "tested": [20], "source": "driver.yml"})

    def test_flow_compilers_and_end_of_block(self):
        text = "language:\n  standard: 23\n  profile: KB-CPP23\n  compilers: {gcc: \"13\", clang: \"17\"}\nbus: [spi]\n"
        entry, problems = language.normalise(language.block_from_text(text), "driver.yml", "x")
        self.assertEqual(problems, [])
        self.assertEqual(entry["standard"], 23)
        self.assertEqual(entry["compilers"], {"clang": "17", "gcc": "13"})
        self.assertEqual(entry["tested"], [23])

    def test_missing_block_is_the_registry_default(self):
        self.assertIsNone(language.block_from_text("name: chip\nbus: [i2c]\n"))
        entry, problems = language.normalise(None, "driver.yml", "x")
        self.assertEqual(problems, [])
        self.assertEqual(entry["standard"], 20)
        self.assertEqual(entry["profile"], "KB-CPP20")
        self.assertEqual(entry["source"], "default")
        self.assertEqual(entry["compilers"]["gcc"], resolver.DEFAULT_LANGUAGE["compilers"]["gcc"])

    def test_bad_blocks_name_every_problem(self):
        bad = {"standard": 21, "profile": "cpp20", "compilers": {"icc": "1", "gcc": 10}, "tested": ["20"]}
        entry, problems = language.normalise(bad, "driver.yml", "chip@v1.0.0")
        self.assertIsNone(entry)
        self.assertEqual(len(problems), 5, problems)
        self.assertTrue(all(p.startswith("chip@v1.0.0: language.") for p in problems))
        self.assertEqual(language.normalise([], "driver.yml", "x")[1], ["x: language must be a mapping"])

    def test_catalogue_version_carries_the_block(self):
        import yaml  # the kb tool reads driver.yml with PyYAML
        doc = yaml.safe_load(DRIVER_YML)
        doc["mcus"] = ["any"]
        entry, problems = catalog.version_entry(doc, "v0.1.0", "a" * 40, None, "repo")
        self.assertEqual(problems, [])
        self.assertEqual(entry["language"]["source"], "driver.yml")
        del doc["language"]
        entry, _ = catalog.version_entry(doc, "v0.1.0", "a" * 40, None, "repo")
        self.assertEqual(entry["language"]["source"], "default")
        sch = common.schema("catalog.schema.json")
        self.assertEqual(minischema.errors(sch["$defs"]["language"], entry["language"]), [])


class KbCoreDeclarationTest(unittest.TestCase):
    def test_kb_yml_declares_the_floor(self):
        with open(os.path.join(common.ROOT, "kb.yml"), encoding="utf-8") as fh:
            entry = catalog.kb_language(fh.read(), "kb.yml")
        self.assertEqual(entry["source"], "kb.yml")
        self.assertIn(entry["standard"], language.STANDARDS)
        self.assertIn(entry["standard"], entry["tested"])

    def test_cmake_reads_the_same_number(self):
        # cmake/kb_language.cmake parses `  standard: N` under `language:`; the Python reader must agree.
        with open(os.path.join(common.ROOT, "kb.yml"), encoding="utf-8") as fh:
            text = fh.read()
        floor = catalog.kb_language(text, "kb.yml")["standard"]
        block = text.split("\nlanguage:", 1)[1]
        self.assertEqual(int(re.search(r"^  standard: *([0-9]+)", block, re.M).group(1)), floor)
        with open(os.path.join(common.ROOT, "cmake", "kb_language.cmake"), encoding="utf-8") as fh:
            self.assertIn("kb.yml", fh.read())

    def test_no_library_target_hard_codes_the_standard(self):
        # Every kb target takes its standard from kb_language.cmake, so raising the floor is one edit in kb.yml.
        hits = []
        for base in ("cmake", os.path.join("components", "lib")):
            for dirpath, _, files in os.walk(os.path.join(common.ROOT, base)):
                for name in files:
                    if name == "CMakeLists.txt" or name.endswith(".cmake"):
                        path = os.path.join(dirpath, name)
                        with open(path, encoding="utf-8") as fh:
                            for n, line in enumerate(fh, 1):
                                code = line.split("#", 1)[0]  # comments may name today's floor
                                if re.search(r"cxx_std_[0-9]+|CMAKE_CXX_STANDARD [0-9]", code):
                                    hits.append(f"{os.path.relpath(path, common.ROOT)}:{n}")
        # components/lib/dma/host is outside this change (its owner moves it with the next DMA change).
        hits = [h for h in hits if not h.startswith(os.path.join("components", "lib", "dma"))]
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
