# Tests of tools/examples: the index parser, the checks on a throwaway tree, and the real repository. Run by ctest
# (kb_examples_unit) and tools/preflight.sh.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "examples"))
import examples  # noqa: E402

LAYOUT = """layout:
  examples: examples
  ports: components/ports
  docs: docs
"""

INDEX = """- id: demo
  title: "demo: x"
  kind: host
  parts: [utils, rtos]
  source: main.cpp
  summary: "does a thing"
  doc: docs/page.md
  snippets: [checks]
"""


class Parse(unittest.TestCase):
    def test_records_lists_and_quotes(self):
        recs = examples.parse(INDEX + "- id: b\n  kind: esp-idf  # trailing\n  parts: []\n")
        self.assertEqual(recs[0]["parts"], ["utils", "rtos"])
        self.assertEqual(recs[0]["title"], "demo: x")
        self.assertEqual(recs[1]["kind"], "esp-idf")
        self.assertEqual(recs[1]["parts"], [])

    def test_garbage_is_refused(self):
        with self.assertRaises(ValueError):
            examples.parse("  id: x\n")
        with self.assertRaises(ValueError):
            examples.parse("- id x\n")


class Tree(unittest.TestCase):
    """A throwaway repository in which every rule can be broken one at a time."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.put("tools/layout/paths.yml", LAYOUT)
        self.put("examples/examples.yml", INDEX)
        self.put("examples/host/demo/main.cpp", "// --8<-- [start:checks]\nint x;\n// --8<-- [end:checks]\n")
        self.put("examples/host/demo/CMakeLists.txt", "")
        self.put("examples/host/demo/README.md", "")
        self.put("docs/page.md", '```cpp\n--8<-- "examples/host/demo/main.cpp:checks"\n```\n')
        self.put("examples/README.md", "<!-- examples:begin -->\nx\n<!-- examples:end -->\n")
        examples.write(self.tmp)

    def put(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def test_consistent_tree_passes(self):
        self.assertEqual(examples.check(self.tmp), [])

    def test_folder_not_in_the_index(self):
        self.put("examples/host/extra/main.cpp", "")
        self.assertTrue(any("extra" in p and "not in examples.yml" in p for p in examples.check(self.tmp)))

    def test_missing_snippet_section(self):
        self.put("examples/host/demo/main.cpp", "int x;\n")
        self.assertTrue(any("no snippet section 'checks'" in p for p in examples.check(self.tmp)))

    def test_page_that_pastes_instead_of_including(self):
        self.put("docs/page.md", "```cpp\nint x;\n```\n")
        self.assertTrue(any("does not include" in p for p in examples.check(self.tmp)))

    def test_stale_table(self):
        self.put("examples/README.md", "<!-- examples:begin -->\nold\n<!-- examples:end -->\n")
        self.assertTrue(any("table is stale" in p for p in examples.check(self.tmp)))

    def test_missing_files_of_a_host_example(self):
        os.unlink(os.path.join(self.tmp, "examples/host/demo/CMakeLists.txt"))
        self.assertTrue(any("CMakeLists.txt does not exist" in p for p in examples.check(self.tmp)))

    def test_app_missing_from_the_index(self):
        self.put("components/ports/p/examples/app_config.yml", "apps:\n  other_app:\n    description: x\n")
        self.assertTrue(any("other_app" in p for p in examples.check(self.tmp)))

    def test_docs_include_of_a_missing_example_section(self):
        self.put("docs/other.md", '--8<-- "examples/host/demo/main.cpp:nope"\n')
        self.assertTrue(any("has no snippet section 'nope'" in p for p in examples.check(self.tmp)))


class Repository(unittest.TestCase):
    def test_the_index_and_everything_that_follows_it_agree(self):
        self.assertEqual(examples.check(ROOT), [])

    def test_every_host_example_is_a_ctest_test_name(self):
        for r in examples.load(ROOT):
            if r["kind"] == "host":
                self.assertRegex(r["id"], r"^[a-z0-9_]+$")


if __name__ == "__main__":
    unittest.main()
