# kb-fetch.sh end to end: local repositories stand in for GitHub (url.<base>.insteadOf), so the script really adds
# the submodules under external/, checks every commit, refuses a moved tag and verifies an existing tree.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""End-to-end test of the emitted fetch script."""

import os
import shutil
import subprocess
import tempfile
import unittest

from kb import emit

from . import gitfixture


@unittest.skipUnless(shutil.which("git") and shutil.which("sh"), "git and sh are needed")
class FetchScriptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = self.tmp.name
        self.github = os.path.join(root, "github")
        kb = gitfixture.driver_repo(os.path.join(self.github, "kokeb-labs", "kb-core.git"), "kb", [("v0.4.0", "0.4.0", "[i2c]")])
        drv = gitfixture.driver_repo(os.path.join(self.github, "kokeb-labs-drivers", "driver-chip.git"), "chip",
                                     [("v0.1.0", "0.1.0", "[i2c]")])
        self.kb_commit = gitfixture.git(kb, "rev-list", "-n", "1", "v0.4.0")
        self.drv_commit = gitfixture.git(drv, "rev-list", "-n", "1", "v0.1.0")
        self.drv = drv
        self.project = os.path.join(root, "project")
        os.makedirs(self.project)
        gitfixture.git(self.project, "init", "-q", "-b", "main")
        self.env = dict(gitfixture.ENV, GIT_CONFIG_COUNT="2",
                        GIT_CONFIG_KEY_0=f"url.file://{self.github}/.insteadOf", GIT_CONFIG_VALUE_0="https://github.com/",
                        GIT_CONFIG_KEY_1="protocol.file.allow", GIT_CONFIG_VALUE_1="always")

    def lock(self, drv_commit=None):
        return {
            "image": "e2e", "kb_core": {"version": "0.4.0", "repo": "kokeb-labs/kb-core", "tag": "v0.4.0",
                                        "commit": self.kb_commit},
            "ports": [{"name": "host", "in_tree": True, "path": "components/lib/interfaces/host", "commit": self.kb_commit}],
            "drivers": [{"name": "chip", "version": "0.1.0", "repo": "kokeb-labs-drivers/driver-chip",
                         "folder": "driver-chip", "tag": "v0.1.0", "commit": drv_commit or self.drv_commit}],
        }

    def run_script(self, lock, *args):
        path = os.path.join(self.project, "kb-fetch.sh")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(emit.fetch_text(lock))
        return subprocess.run(["sh", path, *args], cwd=self.project, env=self.env, capture_output=True, text=True,
                              check=False)

    def test_adds_verifies_and_refuses(self):
        res = self.run_script(self.lock())
        self.assertEqual(res.returncode, 0, res.stderr)
        head = gitfixture.git(os.path.join(self.project, "external", "drivers", "driver-chip"), "rev-parse", "HEAD")
        self.assertEqual(head, self.drv_commit)
        with open(os.path.join(self.project, ".gitmodules"), encoding="utf-8") as fh:
            modules = fh.read()
        self.assertIn("path = external/kb-core", modules)
        self.assertNotIn("branch", modules)
        self.assertEqual(self.run_script(self.lock(), "--verify").returncode, 0)
        # A second run is a no-op that passes.
        self.assertEqual(self.run_script(self.lock()).returncode, 0)
        # A lock whose commit the tag does not name: the tag "moved", the script refuses.
        res = self.run_script(self.lock(drv_commit="0" * 40))
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("the tag moved", res.stderr)

    def test_verify_needs_the_tree(self):
        res = self.run_script(self.lock(), "--verify")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("is missing", res.stderr)


if __name__ == "__main__":
    unittest.main()
