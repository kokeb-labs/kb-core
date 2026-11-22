# The tag mapping of the configurator catalogue (configurator/tools/tags.py, rules in configurator/data/tags.json).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Alias, drop, fallback and counts of the search tags."""

import copy
import json
import os
import sys
import unittest

from . import common

CONF = os.path.join(common.ROOT, "configurator")
sys.path.insert(0, os.path.join(CONF, "tools"))
import tags  # noqa: E402


class TagMappingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rules = tags.load_rules()

    def test_the_shipped_rules_load(self):
        self.assertGreaterEqual(len(self.rules["allow"]), 15)
        self.assertIn("other", self.rules["categories"])

    def test_universal_topics_and_bus_names_are_dropped(self):
        topics = ["cpp20", "driver", "embedded", "header-only", "kokeb-labs", "misra-cpp", "kb-driver",
                  "spi", "i2c", "uart", "can", "modbus"]
        self.assertEqual(tags.tags_from_topics(topics, self.rules), [])

    def test_adc_is_not_a_bus_tag(self):
        self.assertEqual(tags.tags_from_topics(["adc", "spi"], self.rules), ["adc"])

    def test_aliases_map_to_the_allowed_tag(self):
        got = tags.tags_from_topics(["motor-drive", "gate-driver", "mass-flow", "ws2812", "ntc", "se050", "oled"],
                                    self.rules)
        self.assertEqual(got, ["display", "flow-sensor", "led", "motor-driver", "secure-element",
                               "temperature-sensor"])

    def test_chip_vendor_and_unknown_topics_are_not_tags(self):
        self.assertEqual(tags.tags_from_topics(["tmc5160", "trinamic", "shtp", "sensor-fusion"], self.rules), [])

    def test_result_is_sorted_and_without_duplicates(self):
        got = tags.tags_from_topics(["stepper", "motor-driver", "motor-drive", "Stepper"], self.rules)
        self.assertEqual(got, ["motor-driver", "stepper"])

    def test_category_is_the_most_common_one(self):
        self.assertEqual(tags.category_of_tags(["adc", "temperature-sensor"], self.rules), "sensor")
        self.assertEqual(tags.category_of_tags(["motor-driver", "stepper", "led"], self.rules), "motor")
        self.assertIsNone(tags.category_of_tags([], self.rules))

    def test_category_tie_goes_to_the_earlier_category(self):
        self.assertEqual(tags.category_of_tags(["led", "imu"], self.rules), "sensor")

    def test_topics_decide_before_the_fallback(self):
        got = tags.driver_tags(["eeprom", "spi"], {"description": "a temperature chip"}, "", ["spi"], self.rules)
        self.assertEqual(got, {"tags": ["eeprom"], "category": "memory"})

    def test_fallback_uses_description_keywords(self):
        got = tags.driver_tags(["chipname"], {"description": "Barometric pressure sensor"}, "", ["i2c"], self.rules)
        self.assertEqual(got, {"tags": ["pressure-sensor"], "category": "sensor"})

    def test_fallback_uses_the_readme_summary(self):
        got = tags.driver_tags([], {}, "C++20 driver for a 16-channel PWM controller", ["i2c"], self.rules)
        self.assertEqual(got["tags"], ["pwm-controller"])
        self.assertEqual(got["category"], "io")

    def test_fallback_prefers_a_declared_category(self):
        got = tags.driver_tags([], {"category": "radio", "description": "led"}, "", ["spi"], self.rules)
        self.assertEqual(got["category"], "radio")

    def test_fallback_by_bus_then_other(self):
        self.assertEqual(tags.driver_tags([], {}, "", ["adc"], self.rules), {"tags": [], "category": "sensor"})
        self.assertEqual(tags.driver_tags([], {}, "", ["spi"], self.rules), {"tags": [], "category": "other"})
        self.assertEqual(tags.driver_tags([], {}, "", [], self.rules), {"tags": [], "category": "other"})

    def test_topics_not_fetched_use_the_fallback_only(self):
        got = tags.driver_tags(None, {"description": "stepper motor controller"}, "", ["spi"], self.rules)
        self.assertEqual(got["tags"], ["stepper"])
        self.assertEqual(got["category"], "motor")

    def test_counts(self):
        drivers = [{"tags": ["adc", "temperature-sensor"], "category": "sensor"},
                   {"tags": ["adc"], "category": "sensor"},
                   {"tags": [], "category": "other"}]
        index = tags.tag_index(drivers, self.rules)
        self.assertEqual(index["tags"]["adc"], 2)
        self.assertEqual(index["tags"]["temperature-sensor"], 1)
        self.assertEqual(index["tags"]["stepper"], 0)
        self.assertEqual(index["categories"]["sensor"], 2)
        self.assertEqual(index["categories"]["other"], 1)
        self.assertEqual(sorted(index["tags"]), list(index["tags"]))

    def test_rules_with_a_broken_reference_are_refused(self):
        for mutate, text in [
            (lambda r: r["alias"].update({"x": "not-allowed"}), "not in allow"),
            (lambda r: r["alias"].update({"adc": "imu"}), "also in allow"),
            (lambda r: r["category_of"].pop("imu"), "no category_of"),
            (lambda r: r["category_of"].update({"imu": "nonsense"}), "not one of categories"),
            (lambda r: r["drop"].append("imu"), "both allow and drop"),
        ]:
            rules = copy.deepcopy(self.rules)
            mutate(rules)
            path = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"tags-test-{os.getpid()}.json")
            with open(path, "w") as fh:
                json.dump(rules, fh)
            try:
                with self.assertRaises(tags.RulesError) as ctx:
                    tags.load_rules(path)
                self.assertIn(text, str(ctx.exception))
            finally:
                os.remove(path)


class CommittedCatalogueTest(unittest.TestCase):
    """The catalogue on disk carries what the rules allow, with counts that add up."""

    @classmethod
    def setUpClass(cls):
        cls.rules = tags.load_rules()
        cls.catalog = common.load_json(os.path.join(CONF, "data", "catalog.json"))

    def test_every_driver_has_allowed_sorted_tags_and_a_category(self):
        for drv in self.catalog["drivers"]:
            with self.subTest(driver=drv["name"]):
                self.assertEqual(drv["tags"], sorted(set(drv["tags"])))
                self.assertTrue(set(drv["tags"]) <= set(self.rules["allow"]))
                self.assertIn(drv["category"], self.rules["categories"])

    def test_counts_match_the_drivers(self):
        self.assertEqual(self.catalog["tag_index"], tags.tag_index(self.catalog["drivers"], self.rules))


if __name__ == "__main__":
    unittest.main()
