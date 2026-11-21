# Resolver rules the shared vectors do not reach: inputs only YAML can produce (non-string keys, floats) and the
# default-version rules.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Unit tests of kb.resolver."""

import copy
import unittest

from kb import resolver

from . import common


def codes(result):
    return [(e["code"], e["subject"]) for e in result["errors"]]


IMAGE = {"schema": 1, "kb_core": {"version": "0.4.0"}, "mcu": {"family": "host"}}


class ResolverTest(unittest.TestCase):
    def setUp(self):
        self.cat = common.base_catalog()

    def test_minimal(self):
        r = resolver.resolve(self.cat, IMAGE)
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["lock"]["parts"], ["utils", "rtos", "interfaces"])
        self.assertEqual(r["lock"]["image"], "kb-image")
        self.assertEqual(r["lock"]["rtos"], "none")

    def test_schema_as_float_is_the_integer(self):
        img = dict(IMAGE, schema=1.0)
        self.assertTrue(resolver.resolve(self.cat, img)["ok"])
        self.assertEqual(codes(resolver.resolve(self.cat, dict(IMAGE, schema=1.5))), [("KB-RES-001", "schema")])
        self.assertEqual(codes(resolver.resolve(self.cat, dict(IMAGE, schema=True))), [("KB-RES-001", "schema")])
        self.assertEqual(codes(resolver.resolve(self.cat, {k: v for k, v in IMAGE.items() if k != "schema"})),
                         [("KB-RES-001", "schema")])

    def test_non_string_keys(self):
        img = dict(IMAGE)
        img[3] = "x"
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-001", "$")])
        img = dict(IMAGE, buses={1: {"model": "owner", "devices": []}})
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-001", "buses")])

    def test_bad_catalogue(self):
        for cat in (None, [], {"schema": 2}, {"schema": True}):
            self.assertEqual(codes(resolver.resolve(cat, IMAGE)), [("KB-RES-030", "catalog")])

    def test_shape_details(self):
        img = dict(IMAGE, kb_core="0.4.0", mcu={"family": 1}, drivers="pcal9555", buses=[])
        self.assertEqual(codes(resolver.resolve(self.cat, img)),
                         [("KB-RES-001", "buses"), ("KB-RES-001", "drivers"), ("KB-RES-001", "kb_core"),
                          ("KB-RES-001", "mcu.family")])
        img = dict(IMAGE, mcu="esp32")
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-001", "mcu")])
        img = dict(IMAGE, mcu={"family": "esp32", "port": {"version": 1, "x": 0}})
        self.assertEqual(codes(resolver.resolve(self.cat, img)),
                         [("KB-RES-001", "mcu.port.version"), ("KB-RES-022", "mcu.port.x")])
        img = dict(IMAGE, buses={"i2c0": {"model": "owner", "devices": [], "kind": 2}})
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-001", "buses.i2c0.kind")])
        img = dict(IMAGE, drivers=[{"name": "pcal9555", "rev": 1}])
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-022", "drivers[0].rev")])

    def test_driver_without_versions(self):
        cat = copy.deepcopy(self.cat)
        cat["drivers"][2]["versions"] = []
        img = dict(IMAGE, drivers=["pcal9555"])
        self.assertEqual(codes(resolver.resolve(cat, img)), [("KB-RES-011", "pcal9555")])

    def test_pin_that_no_longer_exists_falls_back(self):
        cat = copy.deepcopy(self.cat)
        cat["drivers"][0]["pinned"] = "0.0.9"
        r = resolver.resolve(cat, dict(IMAGE, drivers=["bmp581"]))
        self.assertEqual(r["lock"]["drivers"][0]["version"], "0.2.0")

    def test_newest_rc_when_no_release(self):
        r = resolver.resolve(self.cat, dict(IMAGE, drivers=["tmc5160"]))
        self.assertEqual(r["lock"]["drivers"][0]["tag"], "v0.1.0-rc.2")

    def test_missing_optional_fields(self):
        cat = copy.deepcopy(self.cat)
        for key in ("license", "authors", "peripheral", "kb_core_min", "requires_owner"):
            del cat["drivers"][2]["versions"][1][key]
        r = resolver.resolve(cat, dict(IMAGE, drivers=["pcal9555"], buses={"i2c0": {"model": "guarded",
                                                                                     "devices": ["pcal9555"]}}))
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["lock"]["drivers"][0]["license"], "NOASSERTION")
        self.assertEqual(r["lock"]["drivers"][0]["authors"], [])

    def test_unknown_driver_on_bus_and_unresolved_driver(self):
        img = dict(IMAGE, drivers=["no_chip"], buses={"i2c0": {"model": "owner", "devices": ["no_chip"]}})
        self.assertEqual(codes(resolver.resolve(self.cat, img)), [("KB-RES-009", "no_chip")])

    def test_bus_without_kind(self):
        img = dict(IMAGE, drivers=["pcal9555"], buses={"sensors": {"model": "owner", "devices": ["pcal9555"]}})
        r = resolver.resolve(self.cat, img)
        self.assertTrue(r["ok"])
        self.assertIsNone(r["lock"]["buses"][0]["kind"])

    def test_gate_without_peripheral_and_explicit_kind(self):
        cat = copy.deepcopy(self.cat)
        cat["drivers"][4]["versions"][0]["peripheral"] = None
        r = resolver.resolve(cat, dict(IMAGE, drivers=["ws2812_rmt"]))
        self.assertEqual(codes(r), [("KB-RES-013", "ws2812_rmt")])
        self.assertIn("only on esp32,", r["errors"][0]["message"])
        img = dict(IMAGE, drivers=["pcal9555"], buses={"sensors": {"model": "owner", "kind": "i2c",
                                                                   "devices": ["pcal9555"]}})
        r = resolver.resolve(self.cat, img)
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["lock"]["buses"][0]["kind"], "i2c")

    def test_every_code_is_documented(self):
        with open(f"{common.ROOT}/docs/design/assembler.md", encoding="utf-8") as fh:
            text = fh.read()
        for code in resolver.CODES:
            self.assertIn(code, text)


if __name__ == "__main__":
    unittest.main()
