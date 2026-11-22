# The command line end to end, on temporary files.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.cli."""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from kb import cli, emit

from . import common, gitfixture

IMAGE_YML = """\
schema: 1
image: cli-board
kb_core: {version: 0.4.0}
mcu: {family: esp32, target: esp32c6}
drivers: [pcal9555, {name: tmc5160, version: 0.1.0-rc.1}]
buses:
  i2c0: {model: guarded, devices: [pcal9555]}
  spi1: {model: owner, devices: [tmc5160]}
"""


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = self.tmp.name
        self.catalog = os.path.join(common.VECTORS, "catalogs", "base.json")

    def path(self, name, text=None):
        p = os.path.join(self.dir, name)
        if text is not None:
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(text)
        return p

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_resolve_writes_every_file(self):
        image = self.path("kb-image.yml", IMAGE_YML)
        out_dir = os.path.join(self.dir, "out")
        code, out, err = self.run_cli("resolve", image, "--catalog", self.catalog, "-o", out_dir)
        self.assertEqual(code, 0, err)
        self.assertIn("warning KB-RES-W01 tmc5160", err)
        self.assertEqual(sorted(os.listdir(out_dir)), sorted([emit.CMAKE_FILE, emit.FETCH_FILE, emit.LOCK_FILE,
                                                              emit.PRESETS_FILE]))
        self.assertTrue(os.access(os.path.join(out_dir, "kb-fetch.sh"), os.X_OK))
        code, out, _ = self.run_cli("resolve", image, "--catalog", self.catalog)
        self.assertEqual(code, 0)
        with open(os.path.join(out_dir, "kb-image.lock.yml"), encoding="utf-8") as fh:
            self.assertEqual(out, fh.read())
        return os.path.join(out_dir, "kb-image.lock.yml")

    def test_resolve_failure(self):
        image = self.path("bad.json", json.dumps({"schema": 1, "kb_core": {"version": "0.4.0"},
                                                  "mcu": {"family": "host"}, "drivers": ["ws2812_rmt"]}))
        code, _, err = self.run_cli("resolve", image, "--catalog", self.catalog)
        self.assertEqual(code, cli.EXIT_RESOLVE)
        self.assertIn("error KB-RES-013 ws2812_rmt", err)

    def test_input_errors(self):
        code, _, err = self.run_cli("resolve", self.path("missing.yml"), "--catalog", self.catalog)
        self.assertEqual(code, cli.EXIT_USAGE)
        self.assertIn("missing.yml", err)
        bad_json = self.path("bad.json", "{")
        self.assertEqual(self.run_cli("resolve", bad_json, "--catalog", self.catalog)[0], cli.EXIT_USAGE)
        bad_yml = self.path("bad.yml", "a: [")
        self.assertEqual(self.run_cli("resolve", bad_yml, "--catalog", self.catalog)[0], cli.EXIT_USAGE)
        not_lock = self.path("x.json", "[]")
        self.assertEqual(self.run_cli("sbom", not_lock)[0], cli.EXIT_USAGE)

    def test_yaml_missing(self):
        from kb import yamlio
        with mock.patch.object(yamlio, "yaml", None):
            code, _, err = self.run_cli("resolve", self.path("i.yml", IMAGE_YML), "--catalog", self.catalog)
        self.assertEqual(code, cli.EXIT_USAGE)
        self.assertIn("PyYAML", err)

    def test_sbom(self):
        lock = self.test_resolve_writes_every_file()
        code, out, _ = self.run_cli("sbom", lock)
        self.assertEqual((code, json.loads(out)["bomFormat"]), (0, "CycloneDX"))
        target = os.path.join(self.dir, "s.spdx.json")
        self.assertEqual(self.run_cli("sbom", lock, "--format", "spdx", "-o", target)[0], 0)
        with open(target, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["spdxVersion"], "SPDX-2.3")
        code, out, _ = self.run_cli("sbom", lock, "--created", "2026-11-14T00:00:00Z")
        self.assertEqual(json.loads(out)["metadata"]["timestamp"], "2026-11-14T00:00:00Z")
        with mock.patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "86400"}):
            out = self.run_cli("sbom", lock, "--format", "spdx")[1]
        self.assertEqual(json.loads(out)["creationInfo"]["created"], "1970-01-02T00:00:00Z")

    def test_catalog_build_offline(self):
        drv = gitfixture.driver_repo(os.path.join(self.dir, "driver-chip"), "chip", [("v0.1.0", "0.1.0", "[i2c]")])
        target = os.path.join(self.dir, "cat", "catalog.json")
        code, out, err = self.run_cli("catalog", "build", "--drivers-dir", drv, "-o", target)
        self.assertEqual(code, 0, err)
        with open(target, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["drivers"][0]["name"], "chip")
        code, out, _ = self.run_cli("catalog", "build", "--drivers-dir", drv, "--kb-core-dir", common.ROOT,
                                    "--include-head", "-o", "-")
        self.assertEqual(code, 0)
        # Newest first: the development revision sorts among the tags by its version, not at a fixed end.
        self.assertEqual([v["kind"] for v in json.loads(out)["kb_core"]["versions"]].count("dev"), 1)
        bad = gitfixture.driver_repo(os.path.join(self.dir, "driver-bad"), "bad", [("v0.2.0", "0.1.0", "[i2c]")])
        self.assertEqual(self.run_cli("catalog", "build", "--drivers-dir", bad, "-o", "-")[0], cli.EXIT_RESOLVE)
        self.assertEqual(self.run_cli("catalog", "build", "--drivers-dir", "/nonexistent", "-o", "-")[0],
                         cli.EXIT_USAGE)

    def test_catalog_build_with_platform_facts(self):
        drv = gitfixture.driver_repo(os.path.join(self.dir, "driver-imu"), "imu", [("v0.1.0", "0.1.0", "[i2c]")])
        facts = self.path("platforms.json", json.dumps({"bindings": {"imu": {"requires_owner": True}}}))
        code, out, _ = self.run_cli("catalog", "build", "--drivers-dir", drv, "--platforms", facts, "-o", "-")
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(out)["drivers"][0]["versions"][0]["requires_owner"])
        empty = self.path("empty.json", "[]")
        self.assertEqual(self.run_cli("catalog", "build", "--drivers-dir", drv, "--platforms", empty, "-o", "-")[0], 0)

    def test_sbom_with_catalogue_scans(self):
        lock = self.test_resolve_writes_every_file()
        code, out, _ = self.run_cli("sbom", lock, "--catalog", self.catalog)
        self.assertEqual(code, 0)
        props = json.loads(out)["components"][0]["properties"]
        self.assertIn({"name": "kb:osv", "value": "not scanned"}, props)

    def test_catalog_build_online(self):
        from kb import catalog
        src = {"name": "chip", "repo": "o/driver-chip", "folder": "driver-chip", "pinned": None, "tags": []}
        with mock.patch.object(catalog, "online_repos", return_value=["driver-chip"]), \
                mock.patch.object(catalog, "online_source", return_value=src) as online:
            code, out, _ = self.run_cli("catalog", "build", "--online", "--org", "o", "-o", "-")
        self.assertEqual(code, 0)
        online.assert_called_once_with("o", "driver-chip", {})

    def test_script_and_module_entry_points(self):
        tools = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        res = subprocess.run([sys.executable, os.path.join(tools, "kb", "kb.py"), "--version"], capture_output=True,
                             text=True, cwd=self.dir, check=False)
        self.assertEqual((res.returncode, res.stdout.strip()), (0, "kb 0.1.0"))
        res = subprocess.run([sys.executable, "-m", "kb", "--version"], capture_output=True, text=True, cwd=tools,
                             check=False)
        self.assertEqual((res.returncode, res.stdout.strip()), (0, "kb 0.1.0"))


if __name__ == "__main__":
    unittest.main()
