# Every shared resolver vector through the Python resolver and emitters (configurator/resolver.mjs runs the same
# vectors: tests/resolver/run_js_vectors.mjs). ADR-0014.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The golden vectors of the resolver specification."""

import unittest

from kb import emit, resolver

from . import common, minischema


def _pick(entries):
    return [{"code": e["code"], "subject": e["subject"]} for e in entries]


class VectorTest(unittest.TestCase):
    def test_at_least_twelve_vectors(self):
        self.assertGreaterEqual(len(common.vectors()), 12)

    def test_every_vector(self):
        for name, vec, cat in common.vectors():
            with self.subTest(vector=name):
                got = resolver.resolve(cat, vec["image"])
                actual = {"ok": got["ok"], "errors": _pick(got["errors"]), "warnings": _pick(got["warnings"])}
                if got["ok"]:
                    actual["lock"] = got["lock"]
                    actual["files"] = emit.files(got["lock"])
                self.assertEqual(emit.canonical_json(actual), emit.canonical_json(vec["expect"]))
                for entry in got["errors"] + got["warnings"]:
                    self.assertIn(entry["code"], resolver.CODES)
                    self.assertTrue(entry["message"])

    def test_vectors_cover_every_code(self):
        seen = set()
        for _, vec, _ in common.vectors():
            seen |= {e["code"] for e in vec["expect"]["errors"] + vec["expect"]["warnings"]}
        self.assertEqual(sorted(set(resolver.CODES) - seen), [])

    def test_vectors_are_described(self):
        for name, vec, _ in common.vectors():
            self.assertTrue(vec["description"], name)


class SchemaTest(unittest.TestCase):
    def test_catalogues_match_the_schema(self):
        sch = common.schema("catalog.schema.json")
        for name, _, cat in common.vectors():
            if cat.get("schema") == 1:
                self.assertEqual(minischema.errors(sch, cat), [], name)

    def test_locks_and_images_match_the_schemas(self):
        lock_schema = common.schema("lock.schema.json")
        image_schema = common.schema("kb-image.schema.json")
        for name, vec, _ in common.vectors():
            if vec["expect"]["ok"]:
                self.assertEqual(minischema.errors(lock_schema, vec["expect"]["lock"]), [], name)
                self.assertEqual(minischema.errors(image_schema, vec["image"]), [], name)

    def test_malformed_images_fail_the_schema(self):
        image_schema = common.schema("kb-image.schema.json")
        for name, vec, _ in common.vectors():
            if any(e["code"] in ("KB-RES-001", "KB-RES-021", "KB-RES-022") for e in vec["expect"]["errors"]):
                self.assertNotEqual(minischema.errors(image_schema, vec["image"]), [], name)

    def test_adr_0012_release_lock_matches(self):
        commit = "a" * 40
        release_lock = {
            "schema": 1,
            "kb_core": {"version": "0.4.0", "tag": "v0.4.0", "commit": commit},
            "drivers": [{"name": "pf1550", "repo": "kokeb-labs-drivers/driver-pf1550", "tag": "v0.1.0",
                         "commit": commit, "kind": "release"}],
            "ports": [{"name": "esp32", "path": "components/ports/esp32", "commit": commit}],
            "toolchain": {"cmake": "3.31.6", "esp_idf": "v5.5.1"},
            "soup": [{"name": "freertos-kernel", "version": "10.5.1", "via": "esp-idf"}],
            "files": [{"name": "kb_utils-0.4.0.tar.gz", "sha256": "b" * 64}],
        }
        self.assertEqual(minischema.errors(common.schema("lock.schema.json"), release_lock), [])


if __name__ == "__main__":
    unittest.main()
