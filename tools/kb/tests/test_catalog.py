# `kb catalog build` (tools/kb/catalog.py), offline from local clones and online through a fake gh.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Unit tests of kb.catalog."""

import base64
import json
import os
import tempfile
import unittest
from unittest import mock

from kb import catalog

from . import common, gitfixture, minischema


def raw(name, tags, repo=None):
    return {"name": name, "repo": repo or f"o/driver-{name}", "folder": f"driver-{name}", "pinned": None,
            "tags": tags}


def yml(name, version, **kw):
    doc = {"name": name, "version": version, "bus": ["i2c"], "mcus": ["any"], "peripheral": None,
           "kb_core_min": None, "authors": ["A"]}
    doc.update(kw)
    return doc


class BuildTest(unittest.TestCase):
    def test_versions_sorted_and_checked(self):
        src = raw("chip", [
            {"tag": "v0.1.0", "commit": "1" * 40, "driver_yml": yml("chip", "0.1.0"), "license": gitfixture.MIT},
            {"tag": "v0.2.0-rc.1", "commit": "2" * 40, "driver_yml": yml("chip", "0.2.0"), "license": None},
            {"tag": "nightly", "commit": "3" * 40, "driver_yml": yml("chip", "0.3.0"), "license": None},
            {"tag": "v0.3.0", "commit": "4" * 40, "driver_yml": None, "license": None},
        ])
        cat, problems = catalog.build([src], [])
        self.assertEqual(problems, ["o/driver-chip: no driver.yml at v0.3.0"])
        vers = cat["drivers"][0]["versions"]
        self.assertEqual([v["tag"] for v in vers], ["v0.2.0-rc.1", "v0.1.0"])
        self.assertEqual([v["license"] for v in vers], ["NOASSERTION", "MIT"])
        self.assertEqual(minischema.errors(common.schema("catalog.schema.json"), cat), [])

    def test_problems(self):
        bad = [
            {"tag": "v0.1.0", "commit": "1" * 40, "driver_yml": yml("other", "0.1.0")},
            {"tag": "v0.2.0", "commit": "2" * 40, "driver_yml": yml("chip", "0.1.0")},
            {"tag": "v0.3.0", "commit": "3" * 40, "driver_yml": yml("chip", "0.3.0", bus=["i3c"], mcus=[],
                                                                     kb_core_min="x", authors="A")},
            {"tag": "v0.4.0", "commit": "4" * 40, "driver_yml": ["not", "a", "mapping"]},
        ]
        _, problems = catalog.build([raw("chip", bad), raw("Bad-Name", []), raw("chip", [])], [])
        text = "\n".join(problems)
        for needle in ("name 'other'", "is not 0.2.0", "bus at v0.3.0", "mcus at v0.3.0", "kb_core_min 'x'",
                       "authors at v0.3.0", "not a mapping", "'Bad-Name'", "appears twice"):
            self.assertIn(needle, text)

    def test_options(self):
        doc = yml("chip", "0.1.0", license="Apache-2.0", requires_owner=True, kb_core_min="0.4.0",
                  mcus=["ESP32"], peripheral="rmt", bus=["spi", "i2c"])
        cat, problems = catalog.build([raw("chip", [{"tag": "v0.1.0", "commit": "1" * 40, "driver_yml": doc}])],
                                      [{"version": "0.1.0", "tag": "v0.1.0", "commit": "a" * 40, "kind": "release"}])
        self.assertEqual(problems, [])
        v = cat["drivers"][0]["versions"][0]
        self.assertEqual((v["license"], v["requires_owner"], v["mcus"], v["bus"]),
                         ("Apache-2.0", True, ["esp32"], ["i2c", "spi"]))

    def test_requires_owner_and_bindings(self):
        good = yml("chip", "0.1.0", requires_owner=True, bindings=["chip"])
        bad = yml("chip", "0.2.0", requires_owner="yes", bindings="chip")
        cat, problems = catalog.build([raw("chip", [{"tag": "v0.1.0", "commit": "1" * 40, "driver_yml": good},
                                                    {"tag": "v0.2.0", "commit": "2" * 40, "driver_yml": bad}])], [])
        self.assertEqual(len(problems), 2)
        v = cat["drivers"][0]["versions"][0]
        self.assertEqual((v["requires_owner"], v["bindings"]), (True, ["chip"]))
        self.assertEqual(minischema.errors(common.schema("catalog.schema.json"), cat), [])
        self.assertEqual(minischema.errors(common.schema("driver-yml.schema.json"), good), [])
        self.assertNotEqual(minischema.errors(common.schema("driver-yml.schema.json"), bad), [])

    def test_binding_facts(self):
        cat, _ = catalog.build([raw("imu", [{"tag": "v0.1.0", "commit": "1" * 40, "driver_yml": yml("imu", "0.1.0")}]),
                                raw("adc", [{"tag": "v0.1.0", "commit": "2" * 40, "driver_yml": yml("adc", "0.1.0")}])],
                               [])
        catalog.apply_binding_facts(cat, {"imu": {"requires_owner": True}, "_comment": "x"})
        self.assertEqual([d["versions"][0]["requires_owner"] for d in cat["drivers"]], [False, True])

    def test_detect_license(self):
        self.assertEqual(catalog.detect_license(gitfixture.MIT), "MIT")
        self.assertEqual(catalog.detect_license("BSD"), "NOASSERTION")
        self.assertEqual(catalog.detect_license(None), "NOASSERTION")


class OfflineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def test_local_clones(self):
        gitfixture.driver_repo(os.path.join(self.root, "drivers", "driver-chip"), "chip",
                               [("v0.1.0-rc.1", "0.1.0", "[i2c]"), ("v0.1.0", "0.1.0", "[i2c, spi]"),
                                ("", "0.2.0", "[i2c]")],
                               origin="git@github.com:kokeb-labs-drivers/driver-chip.git")
        gitfixture.driver_repo(os.path.join(self.root, "drivers", "driver-led"), "led",
                               [("v1.0.0", "1.0.0", "[pio]")], license_text=None)
        os.makedirs(os.path.join(self.root, "drivers", "not-a-driver"))
        dirs = catalog.driver_dirs([os.path.join(self.root, "drivers")])
        self.assertEqual([os.path.basename(d) for d in dirs], ["driver-chip", "driver-led"])
        self.assertEqual(catalog.driver_dirs([dirs[0]]), [dirs[0]])
        src = catalog.local_source(dirs[0], "kokeb-labs-drivers", {"chip": "0.1.0"})
        self.assertEqual(src["repo"], "kokeb-labs-drivers/driver-chip")
        self.assertEqual(src["pinned"], "0.1.0")
        self.assertEqual([t["tag"] for t in src["tags"]], ["v0.1.0", "v0.1.0-rc.1"])
        led = catalog.local_source(dirs[1], "acme")
        self.assertEqual(led["repo"], "acme/driver-led")
        cat, problems = catalog.build([src, led], [])
        self.assertEqual(problems, [])
        chip = cat["drivers"][0]
        self.assertEqual(chip["versions"][0]["bus"], ["i2c", "spi"])
        self.assertEqual(chip["versions"][0]["commit"], gitfixture.git(dirs[0], "rev-list", "-n", "1", "v0.1.0"))
        self.assertEqual(cat["drivers"][1]["versions"][0]["license"], "NOASSERTION")

    def test_errors(self):
        with self.assertRaises(catalog.CatalogError):
            catalog.driver_dirs([os.path.join(self.root, "missing")])
        os.makedirs(os.path.join(self.root, "empty"))
        with self.assertRaises(catalog.CatalogError):
            catalog.local_source(os.path.join(self.root, "empty"), "o")
        with open(os.path.join(self.root, "empty", "driver.yml"), "w", encoding="utf-8") as fh:
            fh.write("version: 1.0.0\n")
        with self.assertRaises(catalog.CatalogError):
            catalog.local_source(os.path.join(self.root, "empty"), "o")

    def test_kb_core_versions_and_pins(self):
        kb = os.path.join(self.root, "kb-core")
        gitfixture.driver_repo(kb, "kb", [("v0.1.0", "0.1.0", "[i2c]")])
        os.makedirs(os.path.join(kb, "cmake"))
        os.makedirs(os.path.join(kb, "drivers"))
        with open(os.path.join(kb, "cmake", "kb_version.cmake"), "w", encoding="utf-8") as fh:
            fh.write("set(KB_VERSION_MAJOR 0)\nset(KB_VERSION_MINOR 2)\nset(KB_VERSION_PATCH 0)\n")
        self.assertEqual(catalog.pins(kb), {})
        with open(os.path.join(kb, "drivers", "drivers.lock.yml"), "w", encoding="utf-8") as fh:
            fh.write("drivers:\n  - {name: chip, tag: v0.1.0-rc.2}\n  - {name: bad, tag: main}\n")
        self.assertEqual(catalog.pins(kb), {"chip": "0.1.0-rc.2"})
        vers = catalog.kb_core_versions(kb, include_head=True)
        self.assertEqual([(v["version"], v["kind"]) for v in vers], [("0.1.0", "release"), ("0.2.0", "dev")])
        self.assertEqual(len(catalog.kb_core_versions(kb, include_head=False)), 1)
        with open(os.path.join(kb, "cmake", "kb_version.cmake"), "w", encoding="utf-8") as fh:
            fh.write("set(KB_VERSION_MAJOR 0)\nset(KB_VERSION_MINOR 1)\nset(KB_VERSION_PATCH 0)\n")
        self.assertEqual([v["kind"] for v in catalog.kb_core_versions(kb, include_head=True)], ["release"])
        with open(os.path.join(kb, "cmake", "kb_version.cmake"), "w", encoding="utf-8") as fh:
            fh.write("nothing\n")
        with self.assertRaises(catalog.CatalogError):
            catalog.kb_core_versions(kb, include_head=True)


class OnlineTest(unittest.TestCase):
    """gh api replaced by a fake that serves one repository."""

    @staticmethod
    def endpoint(args):
        return next(a for a in args if a.startswith(("repos/", "orgs/")))

    def fake_gh(self, *args):
        path = self.endpoint(args)
        if path.startswith("orgs/"):
            return "driver-chip\nother-repo\n"
        if "/tags?" in path:
            return "v0.1.0 " + "1" * 40 + "\nmain-snapshot " + "2" * 40 + "\n"
        if "contents/driver.yml" in path:
            text = "name: chip\nversion: 0.1.0\nbus: [i2c]\nmcus: [any]\nauthors: [A]\n"
            return json.dumps({"content": base64.b64encode(text.encode()).decode()})
        return None

    def test_online(self):
        with mock.patch.object(catalog, "_gh", side_effect=self.fake_gh):
            self.assertEqual(catalog.online_repos("o"), ["driver-chip"])
            src = catalog.online_source("o", "driver-chip", {"chip": "0.1.0"})
        self.assertEqual(src["name"], "chip")
        self.assertEqual(src["pinned"], "0.1.0")
        self.assertEqual([t["tag"] for t in src["tags"]], ["v0.1.0"])
        cat, problems = catalog.build([src], [])
        self.assertEqual(problems, [])
        self.assertEqual(cat["drivers"][0]["versions"][0]["license"], "NOASSERTION")

    def test_online_failures(self):
        with mock.patch.object(catalog, "_gh", return_value=None):
            with self.assertRaises(catalog.CatalogError):
                catalog.online_repos("o")
            with self.assertRaises(catalog.CatalogError):
                catalog.online_source("o", "driver-x")

    def test_online_without_driver_yml(self):
        def gh(*args):
            return ("v0.1.0 " + "1" * 40 + "\n") if "/tags?" in OnlineTest.endpoint(args) else None
        with mock.patch.object(catalog, "_gh", side_effect=gh):
            src = catalog.online_source("o", "driver-two-words")
        self.assertEqual(src["name"], "two_words")


if __name__ == "__main__":
    unittest.main()
