# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The port SDK asset: its content, its checks, and a minimal port built from the extracted asset alone."""
from __future__ import annotations

import gzip
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
import port_sdk  # noqa: E402

VERSION = "0.1.0-rc.2"
PREFIX = f"kb-port-sdk-{VERSION}/"


class PortSdk(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.archive, cls.manifest = port_sdk.build(ROOT, os.path.join(cls.tmp, "a"), None, VERSION)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def names(self):
        with tarfile.open(self.archive) as tar:
            return {m.name.removeprefix(PREFIX) for m in tar.getmembers()}

    def test_the_archive_is_reproducible(self):
        again, _ = port_sdk.build(ROOT, os.path.join(self.tmp, "b"), None, VERSION)
        self.assertEqual(open(self.archive, "rb").read(), open(again, "rb").read())

    def test_it_holds_what_a_port_needs_and_nothing_else(self):
        names = self.names()
        for need in ("LICENSE", "CMakeLists.txt", "SDK-MANIFEST.json", "catalog/compat.yml", "tools/layout/paths.yml",
                     "components/lib/interfaces/inc/kb/interfaces/uart.hpp",
                     "components/lib/dma/inc/kb/dma/stream.hpp",
                     "components/lib/dma/host/inc/kb/dma/host/conformance.hpp",
                     "components/lib/interfaces/host/inc/kb/interfaces/host/fake_uart.hpp",
                     "tests/harness/kb_test.hpp"):
            self.assertIn(need, names)
        for top in {n.split("/")[0] for n in names}:
            self.assertIn(top, {"LICENSE", "CMakeLists.txt", "SDK-MANIFEST.json", "kb.yml", "catalog", "cmake",
                                "tools", "components", "tests"})
        for n in names:
            self.assertFalse(n.startswith(("components/drivers/", "components/lib/handlers/", "configurator/",
                                           "docs/", "examples/", ".github/")), n)
            if n.startswith("tests/"):
                self.assertTrue(n.startswith("tests/harness/"), n)

    def test_the_compat_slice_has_the_version_and_no_drivers(self):
        with tarfile.open(self.archive) as tar:
            text = tar.extractfile(PREFIX + "catalog/compat.yml").read().decode()
        self.assertIn("kb_core:", text)
        self.assertNotIn("drivers:", text)
        self.assertNotIn("repo: ", text)

    def test_the_release_manifest_matches_the_archive(self):
        inner = port_sdk.verify(self.archive, self.manifest)
        release = json.load(open(self.manifest, encoding="utf-8"))
        self.assertEqual(release["archive"]["name"], f"kb-port-sdk-{VERSION}.tar.gz")
        self.assertEqual(inner["version"], VERSION)
        self.assertEqual(inner["parts"], ["dma", "interfaces", "rtos", "utils"])
        self.assertIn("dma_conformance", inner["suites"])

    def _rewrite(self, edit):
        """A copy of the archive with `edit(members: dict name->bytes)` applied."""
        with tarfile.open(self.archive) as tar:
            members = {m.name: tar.extractfile(m).read() for m in tar.getmembers()}
        edit(members)
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w") as tar:
            for name, body in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(body)
                tar.addfile(info, io.BytesIO(body))
        path = os.path.join(self.tmp, "edited.tar.gz")
        with gzip.open(path, "wb") as f:
            f.write(raw.getvalue())
        return path

    def test_verify_refuses_a_changed_file(self):
        path = self._rewrite(lambda m: m.__setitem__(PREFIX + "LICENSE", b"changed"))
        with self.assertRaisesRegex(port_sdk.SdkError, "LICENSE differs"):
            port_sdk.verify(path, None)

    def test_verify_refuses_an_extra_file_and_an_unsafe_name(self):
        path = self._rewrite(lambda m: m.__setitem__(PREFIX + "extra.txt", b"x"))
        with self.assertRaisesRegex(port_sdk.SdkError, "differ from the manifest"):
            port_sdk.verify(path, None)
        path = self._rewrite(lambda m: m.__setitem__(PREFIX + "../escape", b"x"))
        with self.assertRaisesRegex(port_sdk.SdkError, "unsafe"):
            port_sdk.verify(path, None)

    def test_verify_refuses_another_archive_than_the_release_manifest_names(self):
        path = self._rewrite(lambda m: None)
        with self.assertRaisesRegex(port_sdk.SdkError, "SHA-256 differs"):
            port_sdk.verify(path, self.manifest)

    def test_a_tag_must_match_the_version_in_cmake(self):
        with self.assertRaisesRegex(port_sdk.SdkError, "bump it"):
            port_sdk.build(ROOT, os.path.join(self.tmp, "c"), "v9.9.9", None)
        with self.assertRaisesRegex(port_sdk.SdkError, "not X.Y.Z"):
            port_sdk.build(ROOT, os.path.join(self.tmp, "c"), None, "1.2")

    @unittest.skipUnless(shutil.which("cmake"), "needs cmake")
    def test_a_minimal_port_builds_from_the_extracted_asset_and_passes_the_conformance_suite(self):
        """Nothing from the kb-core checkout but the extracted archive: configure, build, run."""
        work = os.path.join(self.tmp, "port")
        os.makedirs(work)
        with tarfile.open(self.archive) as tar:
            # verify() already refused unsafe names; the data filter (3.12+) also drops special modes.
            kwargs = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
            tar.extractall(os.path.join(work, "sdk"), **kwargs)
        sdk = os.path.join(work, "sdk", PREFIX.rstrip("/"))
        src = os.path.join(work, "port-src")
        shutil.copytree(os.path.join(ROOT, "tests", "port_sdk", "minimal_port"), src)
        build = os.path.join(work, "build")
        cfg = ["cmake", "-S", src, "-B", build, f"-DKB_SDK_DIR={sdk}", "-DCMAKE_BUILD_TYPE=Debug"]
        if os.environ.get("KB_PORT_SDK_CXX"):
            cfg.append(f"-DCMAKE_CXX_COMPILER={os.environ['KB_PORT_SDK_CXX']}")
        for step in (cfg, ["cmake", "--build", build, "-j", "4"]):
            out = subprocess.run(step, capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout[-3000:] + out.stderr[-3000:])
        exe = os.path.join(build, "minimal_port")
        out = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("4 test cases, 0 failed", out.stdout)


if __name__ == "__main__":
    unittest.main()
