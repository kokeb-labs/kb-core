# `kb sbom`: CycloneDX 1.6 and SPDX 2.3 from a lock.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Unit tests of kb.sbom."""

import copy
import os
import subprocess
import unittest

from kb import resolver, sbom

from . import common


def lock_of(vector_name):
    for name, vec, _ in common.vectors():
        if name == vector_name:
            return vec["expect"]["lock"]
    raise KeyError(vector_name)


class SbomTest(unittest.TestCase):
    def test_cyclonedx(self):
        lock = lock_of("002-esp32-drivers.json")
        doc = sbom.cyclonedx(lock)
        self.assertEqual((doc["bomFormat"], doc["specVersion"], doc["version"]), ("CycloneDX", "1.6", 1))
        self.assertNotIn("timestamp", doc["metadata"])
        refs = [c["bom-ref"] for c in doc["components"]]
        self.assertEqual(refs, ["kb-core", "driver-pcal9555", "driver-ws2812_rmt", "soup:esp-idf",
                                "soup:freertos-kernel"])
        self.assertIn({"name": "kb:osv", "value": "not scanned"}, doc["components"][0]["properties"])
        self.assertNotIn("kb:osv", [p["name"] for p in doc["components"][0]["components"][0]["properties"]])
        self.assertEqual(doc["components"][4]["licenses"], [{"license": {"id": "MIT"}}])
        kb = doc["components"][0]
        self.assertEqual([c["bom-ref"] for c in kb["components"]],
                         ["kb-core/utils", "kb-core/rtos", "kb-core/interfaces", "kb-core/handlers",
                          "kb-core/port-esp32"])
        self.assertEqual(kb["purl"], "pkg:github/kokeb-labs/kb-core@v0.4.0")
        self.assertEqual(doc["components"][1]["licenses"], [{"license": {"id": "MIT"}}])
        self.assertEqual(doc["dependencies"][0]["dependsOn"], sorted(refs))
        self.assertTrue(doc["serialNumber"].startswith("urn:uuid:"))
        self.assertEqual(sbom.cyclonedx(lock), doc)
        self.assertEqual(sbom.cyclonedx(lock, "2026-11-14T00:00:00Z")["metadata"]["timestamp"], "2026-11-14T00:00:00Z")

    def test_serial_follows_the_lock(self):
        lock = lock_of("002-esp32-drivers.json")
        other = copy.deepcopy(lock)
        other["drivers"][0]["commit"] = "0" * 40
        self.assertNotEqual(sbom.cyclonedx(lock)["serialNumber"], sbom.cyclonedx(other)["serialNumber"])

    def test_spdx(self):
        lock = lock_of("002-esp32-drivers.json")
        doc = sbom.spdx(lock)
        self.assertEqual((doc["spdxVersion"], doc["dataLicense"]), ("SPDX-2.3", "CC0-1.0"))
        self.assertEqual(doc["creationInfo"]["created"], sbom.EPOCH)
        ids = [p["SPDXID"] for p in doc["packages"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn("SPDXRef-driver-ws2812-rmt", ids)
        self.assertEqual(doc["relationships"][0]["relationshipType"], "DESCRIBES")
        rels = {(r["spdxElementId"], r["relatedSpdxElement"]) for r in doc["relationships"][1:]}
        self.assertIn(("SPDXRef-kb-core", "SPDXRef-kb-core-utils"), rels)
        self.assertIn(("SPDXRef-image-example-board-a", "SPDXRef-driver-pcal9555"), rels)
        self.assertTrue(doc["documentNamespace"].endswith(sbom.lock_digest(lock)))
        self.assertEqual(sbom.spdx(lock, "2026-01-01T00:00:00Z")["creationInfo"]["created"], "2026-01-01T00:00:00Z")

    def test_extracted_port_and_dev_kb_core(self):
        lock = lock_of("019-extracted-port.json")
        comps = sbom.components(lock)
        self.assertIn("port-esp32", [c["ref"] for c in comps])
        dev = lock_of("022-dev-kb-core.json")
        doc = sbom.cyclonedx(dev)
        self.assertEqual(doc["components"][0]["purl"], f"pkg:github/kokeb-labs/kb-core@{dev['kb_core']['commit']}")
        spdx = sbom.spdx(dev)
        self.assertIn(dev["kb_core"]["commit"], spdx["packages"][1]["downloadLocation"])

    def test_unknown_licence_and_no_authors(self):
        r = resolver.resolve(common.base_catalog(), {"schema": 1, "kb_core": {"version": "0.4.0"},
                                                     "mcu": {"family": "host"}, "drivers": ["pcal9555"]})
        lock = copy.deepcopy(r["lock"])
        lock["drivers"][0]["license"] = "NOASSERTION"
        lock["drivers"][0]["authors"] = []
        drv = sbom.cyclonedx(lock)["components"][1]
        self.assertNotIn("licenses", drv)
        self.assertNotIn("authors", drv)
        self.assertEqual(sbom.spdx(lock)["packages"][-1]["copyrightText"], "NOASSERTION")


    def test_soup(self):
        def lock(family, rtos):
            return {"mcu": {"family": family}, "rtos": rtos}
        self.assertEqual([s["name"] for s in sbom.soup(lock("esp32", "freertos"))], ["esp-idf", "freertos-kernel"])
        self.assertEqual(sbom.soup(lock("esp32", "freertos"))[1]["note"], "RTOS, via esp-idf")
        self.assertEqual(sbom.soup(lock("stm32", "freertos"))[0]["note"], "RTOS backend")
        self.assertEqual([s["name"] for s in sbom.soup(lock("stm32", "threadx"))], ["threadx"])
        threadx = sbom.soup(lock("stm32", "threadx"))[0]
        self.assertIsNone(threadx["version"])  # the image's kernel may be the product's
        self.assertIn(sbom.THREADX_PIN, threadx["note"])
        # The pin named is the one the SOUP list records, and the submodule's gitlink is that record's commit.
        with open(os.path.join(common.ROOT, "docs", "assurance", "soup.md"), encoding="utf-8") as fh:
            soup_md = fh.read()
        self.assertIn(f"`{sbom.THREADX_PIN}`", soup_md)
        tree = subprocess.run(["git", "-C", common.ROOT, "ls-tree", "HEAD", "third_party/threadx"],
                              capture_output=True, text=True, check=False).stdout.split()
        if len(tree) >= 3:  # not in an export without the submodule
            self.assertIn(f"commit `{tree[2]}`", soup_md)
        self.assertEqual(sbom.soup(lock("host", "none")), [])
        spdx = sbom.spdx(lock_of("002-esp32-drivers.json"))
        self.assertEqual([p["name"] for p in spdx["packages"][-2:]], ["esp-idf", "freertos-kernel"])
        self.assertEqual(spdx["packages"][-1]["versionInfo"], "NOASSERTION")

    def test_osv_from_a_catalogue(self):
        name, vec, cat = next(v for v in common.vectors() if v[0] == "019-extracted-port.json")
        cat = copy.deepcopy(cat)
        scan = {"scanned_at": "2026-11-14", "database": "osv.dev", "findings": [{"id": "GHSA-x", "severity": "low"}]}
        cat["drivers"][0]["versions"][0]["osv"] = scan  # bmp581 0.2.0
        cat["kb_core"]["versions"][1]["osv"] = None  # 0.4.0-rc.1
        cat["mcus"][0]["port"]["versions"][0]["osv"] = scan  # port 0.5.0
        lock = vec["expect"]["lock"]
        doc = sbom.cyclonedx(lock, None, cat)
        props = {c["bom-ref"]: {p["name"]: p["value"] for p in c["properties"]} for c in doc["components"]}
        self.assertEqual(props["driver-bmp581"]["kb:osv"], "scanned 2026-11-14 (osv.dev), 1 finding(s)")
        self.assertEqual(props["port-esp32"]["kb:osv"], "scanned 2026-11-14 (osv.dev), 1 finding(s)")
        self.assertEqual(props["kb-core"]["kb:osv"], "not scanned")
        spdx = sbom.spdx(lock, None, cat)
        comments = {p["name"]: p.get("comment", "") for p in spdx["packages"]}
        self.assertTrue(comments["driver-bmp581"].endswith("known-vulnerability scan: 2026-11-14"))
        self.assertTrue(comments["kb-core"].endswith("known-vulnerability scan: not scanned"))
        self.assertNotIn("scan", comments["kb-utils"])
        other = copy.deepcopy(lock)
        other["drivers"][0]["version"] = "9.9.9"
        self.assertIsNone(sbom.osv_index(cat, other)["driver-bmp581"])


if __name__ == "__main__":
    unittest.main()
