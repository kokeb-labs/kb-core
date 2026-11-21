# Versions and tags (ADR-0012).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.semver."""

import unittest

from kb import semver


class SemverTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(semver.parse_version("1.2.3")[:3], (1, 2, 3))
        self.assertEqual(semver.parse_version("1.2.3-rc.4"), (1, 2, 3, 4))
        for bad in ("1.2", "01.2.3", "1.2.3-rc", "1.2.3-beta.1", "v1.2.3", "", None, 3, "1.2.3-rc.01"):
            self.assertIsNone(semver.parse_version(bad), bad)

    def test_order(self):
        ordered = ["0.1.0-rc.1", "0.1.0-rc.2", "0.1.0-rc.10", "0.1.0", "0.1.1", "0.2.0", "1.0.0-rc.1", "1.0.0"]
        for a, b in zip(ordered, ordered[1:]):
            self.assertEqual(semver.compare(a, b), -1, (a, b))
            self.assertEqual(semver.compare(b, a), 1, (a, b))
        self.assertEqual(semver.compare("0.4.0", "0.4.0"), 0)
        with self.assertRaises(ValueError):
            semver.compare("x", "0.1.0")

    def test_tags(self):
        self.assertTrue(semver.is_tag("v0.1.0"))
        self.assertTrue(semver.is_tag("v0.1.0-rc.1"))
        for bad in ("0.1.0", "main", "v0.1", "v0.1.0-rc", "release/v0.1.0", None):
            self.assertFalse(semver.is_tag(bad), bad)
        self.assertEqual(semver.tag_kind("v1.0.0"), "release")
        self.assertEqual(semver.tag_kind("v1.0.0-rc.3"), "rc")
        self.assertIsNone(semver.tag_kind("main"))


if __name__ == "__main__":
    unittest.main()
