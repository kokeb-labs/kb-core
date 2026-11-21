# availability(): which drivers a target, RTOS and language standard can use, from the site catalogue alone.
# The shared vectors (033 and up) pin the answer for both resolvers; these tests cover the real catalogue.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Availability of drivers."""

import copy
import os
import unittest

from kb import resolver

from . import common

SITE = common.load_json(os.path.join(common.ROOT, "configurator", "data", "catalog.json"))


def unavailable(result):
    return {u["name"]: u for u in result["unavailable"]}


class AvailabilityTest(unittest.TestCase):
    def test_defaults_of_a_family(self):
        got = resolver.availability(SITE, {"family": "esp32"})
        names = sorted(d["name"] for d in SITE["drivers"])
        self.assertEqual(got["unavailable"], [])
        self.assertEqual(got["available"], names)

    def test_every_driver_is_in_exactly_one_list(self):
        for sel in ({"family": "esp32"}, {"family": "host"}, {"family": "stm32"}, {"family": "esp32", "rtos": "none"}):
            got = resolver.availability(SITE, sel)
            listed = got["available"] + [u["name"] for u in got["unavailable"]]
            self.assertEqual(sorted(listed), sorted(d["name"] for d in SITE["drivers"]), sel)

    def test_the_esp32_only_driver_is_not_offered_on_the_host(self):
        got = unavailable(resolver.availability(SITE, {"family": "host"}))
        self.assertEqual(got["ws2812_rmt"]["code"], "KB-RES-013")
        self.assertEqual(len(got), 1)

    def test_a_planned_family_has_no_port(self):
        got = resolver.availability(SITE, {"family": "stm32"})
        self.assertEqual(got["available"], [])
        self.assertEqual({u["code"] for u in got["unavailable"]}, {"KB-RES-004"})
        self.assertIn("no port", got["unavailable"][0]["reason"])

    def test_the_language_standard_is_judged_against_the_drivers(self):
        got = resolver.availability(SITE, {"family": "esp32", "language": {"standard": 17}})
        self.assertEqual({u["code"] for u in got["unavailable"]}, {"KB-RES-024"})
        self.assertEqual(len(got["unavailable"]), len(SITE["drivers"]))
        self.assertIn("needs C++20, project is C++17", got["unavailable"][0]["reason"])

    def test_a_peripheral_the_target_lacks(self):
        cat = copy.deepcopy(SITE)
        for fam in cat["families"]:
            if fam["id"] == "esp32":
                fam["targets"][5]["peripherals"] = []
        got = unavailable(resolver.availability(cat, {"family": "esp32", "target": "esp32c6"}))
        self.assertEqual(got["ws2812_rmt"]["code"], "KB-RES-031")
        self.assertIn("RMT", got["ws2812_rmt"]["reason"])

    def test_a_catalogue_without_requires_reads_the_newest_version(self):
        cat = copy.deepcopy(SITE)
        for drv in cat["drivers"]:
            del drv["requires"]
        self.assertEqual(resolver.availability(cat, {"family": "host"}), resolver.availability(SITE, {"family": "host"}))

    def test_an_unknown_target_blocks_every_driver(self):
        got = resolver.availability(SITE, {"family": "esp32", "target": "nope"})
        self.assertEqual({u["code"] for u in got["unavailable"]}, {"KB-RES-005"})

    def test_codes_are_documented(self):
        for sel in ({"family": "host"}, {"family": "stm32"}, {"family": "esp32", "language": {"standard": 17}}):
            for u in resolver.availability(SITE, sel)["unavailable"]:
                self.assertIn(u["code"], resolver.AVAILABILITY_CODES)


if __name__ == "__main__":
    unittest.main()
