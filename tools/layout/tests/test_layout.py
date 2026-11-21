# Tests of tools/layout: the layout file, the root allowlist, the stale-reference scan, and that CMake reads the same
# values as Python. Run by ctest (kb_layout_unit) and tools/preflight.sh.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "layout"))
import layout  # noqa: E402


def in_git_checkout():
    return subprocess.run(["git", "-C", ROOT, "rev-parse", "--git-dir"], capture_output=True).returncode == 0


class LayoutFile(unittest.TestCase):
    def setUp(self):
        self.data = layout.load()

    def test_sections(self):
        for section in ("layout", "retired", "root_dirs", "root_files"):
            self.assertTrue(self.data[section], section)

    def test_every_layout_entry_is_a_folder(self):
        for name, rel in self.data["layout"].items():
            self.assertTrue(os.path.isdir(os.path.join(ROOT, rel)), f"{name}: {rel}")

    def test_retired_entries_name_a_layout_entry(self):
        for old, name in self.data["retired"].items():
            self.assertIn(name, self.data["layout"], old)

    def test_get(self):
        self.assertEqual(layout.get("lib", data=self.data), self.data["layout"]["lib"])
        self.assertTrue(os.path.isabs(layout.get("lib", absolute=True, data=self.data)))
        with self.assertRaises(KeyError):
            layout.get("nope", data=self.data)

    def test_grouping_the_owner_asked_for(self):
        lay = self.data["layout"]
        parent = os.path.dirname(lay["lib"])
        self.assertEqual({os.path.dirname(lay["ports"]), os.path.dirname(lay["drivers"])}, {parent})
        self.assertEqual(os.path.dirname(lay["host_port"]), lay["ports"])

    def test_parser_rejects_garbage(self):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as f:
            f.write("layout:\n  lib components/lib\n")
        try:
            with self.assertRaises(ValueError):
                layout.load(f.name)
        finally:
            os.unlink(f.name)


class RootAllowlist(unittest.TestCase):
    def setUp(self):
        self.data = layout.load()

    @unittest.skipUnless(in_git_checkout(), "needs a git checkout")
    def test_the_repository_root_is_clean(self):
        self.assertEqual(layout.unexpected_root_entries(layout.root_entries(ROOT), self.data), [])
        self.assertEqual(layout.missing_root_entries(ROOT, self.data), [])

    def test_unexpected_folder_and_file_are_reported(self):
        tracked = ["README.md", "components/lib/x.hpp", "stray/file.txt", "notes.txt", ".github/CODEOWNERS"]
        self.assertEqual(layout.unexpected_root_entries(tracked, self.data), ["notes.txt", "stray/"])

    def test_old_top_level_folders_are_unexpected(self):
        for old in self.data["retired"]:
            top = old.split("/")[0]
            if top not in self.data["root_dirs"]:
                self.assertEqual(layout.unexpected_root_entries([f"{top}/a"], self.data), [f"{top}/"])

    def test_cli_exit_codes(self):
        tmp = tempfile.mkdtemp()
        try:
            self._make_repo(tmp)
            cli = [sys.executable, "-I", os.path.join(ROOT, "tools", "layout", "layout.py"), "--root", tmp, "root"]
            self.assertEqual(subprocess.run(cli, capture_output=True).returncode, 0)
            with open(os.path.join(tmp, "stray.txt"), "w") as f:
                f.write("x")
            subprocess.run(["git", "-C", tmp, "add", "stray.txt"], check=True)
            out = subprocess.run(cli, capture_output=True, text=True)
            self.assertEqual(out.returncode, 1)
            self.assertIn("stray.txt", out.stdout)
        finally:
            shutil.rmtree(tmp)

    def test_untracked_folders_are_not_part_of_the_root(self):
        """A job or a tool leaves reports/ (untracked, not ignored) or build/ (ignored) in the root: not a finding."""
        tmp = tempfile.mkdtemp()
        try:
            self._make_repo(tmp)
            with open(os.path.join(tmp, ".gitignore"), "w") as f:
                f.write("build/\n")
            subprocess.run(["git", "-C", tmp, "add", ".gitignore"], check=True)
            for rel in ("reports/out.xml", "build/obj.o", "scratch.txt"):
                os.makedirs(os.path.dirname(os.path.join(tmp, rel)), exist_ok=True)
                with open(os.path.join(tmp, rel), "w") as f:
                    f.write("x")
            self.assertEqual(layout.unexpected_root_entries(layout.root_entries(tmp), self.data), [])
            cli = [sys.executable, "-I", os.path.join(ROOT, "tools", "layout", "layout.py"), "--root", tmp, "root"]
            self.assertEqual(subprocess.run(cli, capture_output=True).returncode, 0)
            # The same folder, once tracked, is a finding.
            subprocess.run(["git", "-C", tmp, "add", "reports"], check=True)
            self.assertEqual(layout.unexpected_root_entries(layout.root_entries(tmp), self.data), ["reports/"])
        finally:
            shutil.rmtree(tmp)

    def test_outside_a_git_checkout_the_directory_listing_is_used(self):
        tmp = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(tmp, "stray"))
            os.makedirs(os.path.join(tmp, "docs"))
            for name in ("README.md", "notes.txt"):
                with open(os.path.join(tmp, name), "w") as f:
                    f.write("x")
            self.assertEqual(sorted(layout.root_entries(tmp)), ["README.md", "docs/x", "notes.txt", "stray/x"])
            self.assertEqual(layout.unexpected_root_entries(layout.root_entries(tmp), self.data), ["notes.txt", "stray/"])
        finally:
            shutil.rmtree(tmp)

    def _make_repo(self, tmp):
        """A throwaway repository with this layout file, every layout folder and every allowed root entry."""
        os.makedirs(os.path.join(tmp, "tools", "layout"))
        shutil.copy(layout.PATHS_YML, os.path.join(tmp, "tools", "layout", "paths.yml"))
        for rel in list(self.data["layout"].values()) + self.data["root_dirs"]:
            os.makedirs(os.path.join(tmp, rel), exist_ok=True)
            with open(os.path.join(tmp, rel, "keep"), "w") as f:
                f.write("")
        for name in self.data["root_files"]:
            with open(os.path.join(tmp, name), "w") as f:
                f.write("")
        subprocess.run(["git", "-C", tmp, "init", "-q"], check=True)
        subprocess.run(["git", "-C", tmp, "add", "-A"], check=True)


class StaleReferences(unittest.TestCase):
    def setUp(self):
        self.data = layout.load()

    @unittest.skipUnless(in_git_checkout(), "needs a git checkout")
    def test_no_tracked_file_names_a_retired_location(self):
        self.assertEqual(layout.stale_references(ROOT, self.data), [])

    def _hits(self, text):
        return [old for p, old, _ in layout.stale_patterns(self.data) if p.search(text)]

    def test_retired_prefixes_are_found(self):
        self.assertEqual(self._hits("see lib/utils/inc"), ["lib"])
        self.assertEqual(self._hits("`ports/esp32/`"), ["ports"])
        self.assertEqual(self._hits("(drivers/drivers.lock.yml)"), ["drivers"])
        self.assertEqual(self._hits("https://github.com/o/kb-core/blob/main/schemas/lock.schema.json"), ["schemas"])
        self.assertEqual(self._hits("external/kb-core/ports/esp32"), ["ports"])

    def test_current_and_foreign_paths_are_not(self):
        for text in ("components/lib/utils", "catalog/schemas/x.json", "/usr/lib/x", "node_modules/lib/x",
                     "kokeb-labs-drivers/port-esp32", "external/drivers/driver-x", "https://x.io/ports/y",
                     "build/lib/utils", "KB_LIB_DIR=lib/x"):
            self.assertEqual(self._hits(text), [], text)

    def test_fix_rewrites_and_a_second_run_is_clean(self):
        tmp = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(tmp, "tools", "layout"))
            shutil.copy(layout.PATHS_YML, os.path.join(tmp, "tools", "layout", "paths.yml"))
            with open(os.path.join(tmp, "doc.md"), "w") as f:
                f.write("Headers: lib/utils/inc and ports/esp32/src, lock drivers/drivers.lock.yml\n")
            subprocess.run(["git", "-C", tmp, "init", "-q"], check=True)
            subprocess.run(["git", "-C", tmp, "add", "-A"], check=True)
            self.assertEqual(len(layout.stale_references(tmp, self.data)), 3)
            layout.stale_references(tmp, self.data, fix=True)
            with open(os.path.join(tmp, "doc.md")) as f:
                text = f.read()
            lay = self.data["layout"]
            self.assertIn(f"{lay['lib']}/utils/inc", text)
            self.assertIn(f"{lay['ports']}/esp32/src", text)
            self.assertIn(f"{lay['drivers']}/drivers.lock.yml", text)
            self.assertEqual(layout.stale_references(tmp, self.data), [])
        finally:
            shutil.rmtree(tmp)


class ReadersAgree(unittest.TestCase):
    """CMake, Python and the hand-kept data files name the same places."""

    @unittest.skipUnless(shutil.which("cmake"), "needs cmake")
    def test_cmake_reads_the_same_values(self):
        data = layout.load()
        tmp = tempfile.mkdtemp()
        try:
            names = list(data["layout"])
            with open(os.path.join(tmp, "CMakeLists.txt"), "w") as f:
                f.write("cmake_minimum_required(VERSION 3.24)\nproject(t NONE)\n")
                f.write(f'include("{ROOT}/cmake/kb_layout.cmake")\n')
                for n in names:
                    up = n.upper()
                    f.write(f'kb_layout({n} abs_{n})\nkb_layout_relative({n} rel_{n})\n')
                    f.write(f'message(STATUS "LAYOUT {n}|${{rel_{n}}}|${{abs_{n}}}|${{KB_{up}_REL}}|${{KB_{up}_DIR}}")\n')
            out = subprocess.run(["cmake", "-S", tmp, "-B", os.path.join(tmp, "b")], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            seen = {}
            for line in out.stdout.splitlines():
                if "LAYOUT " in line:
                    n, rel, absolute, rel2, abs2 = line.split("LAYOUT ", 1)[1].split("|")
                    seen[n] = (rel, absolute, rel2, abs2)
            self.assertEqual(sorted(seen), sorted(names))
            for n in names:
                rel, absolute, rel2, abs2 = seen[n]
                self.assertEqual(rel, data["layout"][n])
                self.assertEqual(rel2, rel)
                self.assertEqual(absolute, layout.get(n, True, data))
                self.assertEqual(abs2, absolute)
        finally:
            shutil.rmtree(tmp)

    @unittest.skipUnless(shutil.which("cmake"), "needs cmake")
    def test_cmake_refuses_an_unknown_entry(self):
        tmp = tempfile.mkdtemp()
        try:
            with open(os.path.join(tmp, "CMakeLists.txt"), "w") as f:
                f.write(f'cmake_minimum_required(VERSION 3.24)\nproject(t NONE)\ninclude("{ROOT}/cmake/kb_layout.cmake")\n'
                        "kb_layout(nonsense x)\n")
            out = subprocess.run(["cmake", "-S", tmp, "-B", os.path.join(tmp, "b")], capture_output=True, text=True)
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("no layout entry", out.stderr)
        finally:
            shutil.rmtree(tmp)

    @unittest.skipUnless(shutil.which("cmake"), "needs cmake")
    def test_a_vendored_subset_configures_and_finds_its_parts(self):
        """What a consumer that keeps a pinned copy of kb-core copies (`vendor`) is enough to configure it."""
        data = layout.load()
        tmp = tempfile.mkdtemp()
        try:
            copy = os.path.join(tmp, "kb-core")
            for rel in layout.vendor_paths(data):
                src = os.path.join(ROOT, rel)
                self.assertTrue(os.path.exists(src), rel)
                dst = os.path.join(copy, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.isdir(src):
                    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("README.md", "tests", "fuzz"))
                else:
                    shutil.copy2(src, dst)
            out = subprocess.run(["cmake", "-S", copy, "-B", os.path.join(tmp, "b"), "-DKB_PARTS=interfaces"],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn("Parts:   utils, rtos, interfaces", out.stdout)
            # Without the layout file the copy says what is missing instead of failing obscurely.
            os.unlink(os.path.join(copy, "tools", "layout", "paths.yml"))
            out = subprocess.run(["cmake", "-S", copy, "-B", os.path.join(tmp, "b2")], capture_output=True, text=True)
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("is missing: cmake/kb_layout.cmake reads it", " ".join(out.stderr.split()))
        finally:
            shutil.rmtree(tmp)

    def test_platform_facts_use_the_layout(self):
        """configurator/data/platforms.json carries the in-tree port paths: they must be the layout's."""
        with open(os.path.join(ROOT, "configurator", "data", "platforms.json"), encoding="utf-8") as f:
            platforms = json.load(f)
        lay = layout.load()["layout"]
        want = {"esp32": f"{lay['ports']}/esp32", "host": f"{lay['lib']}/interfaces/host"}
        got = {fam["id"]: fam["port"]["path"] for fam in platforms["families"] if fam.get("port")}
        for family, path in want.items():
            self.assertEqual(got[family], path)


if __name__ == "__main__":
    unittest.main()
