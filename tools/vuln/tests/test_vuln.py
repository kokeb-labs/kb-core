# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""scan.py and collect.py against fixtures (no network), and the committed snapshot."""

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
VULN = HERE.parent
ROOT = VULN.parent.parent
sys.path.insert(0, str(VULN))

import collect  # noqa: E402
import issue_body  # noqa: E402
import scan  # noqa: E402
from kb import sbom  # noqa: E402

FIX = HERE / "fixtures"
DATA = ROOT / "configurator" / "data"


def run(module, *argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = module.main(list(map(str, argv)))
    return rc, out.getvalue()


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))

    def scan(self, fixture="osv.json", date="2026-11-21"):
        out = self.tmp / "v.json"
        rc, text = run(scan, "--root", ROOT, "--offline-fixture", FIX / fixture, "--out", out, "--date", date)
        return rc, text, out

    def test_found(self):
        rc, text, out = self.scan()
        self.assertEqual(rc, 0, text)
        doc = json.loads(out.read_text())
        self.assertEqual(scan.problems(doc), [])
        self.assertEqual(doc["status"], "advisories open")
        self.assertEqual(doc["checked_at"], "2026-11-21")
        self.assertEqual(doc["source"], "OSV")
        by = {c["name"]: c for c in doc["components"]}
        self.assertEqual(by["threadx"]["count"], 2)
        self.assertEqual(by["threadx"]["commit"], "1" * 40)
        self.assertEqual(by["esp-idf"]["commit"], "2" * 40)
        self.assertEqual(by["driver-ads7952"]["status"], "advisories open")
        self.assertEqual(doc["totals"]["components"], len(doc["components"]))
        self.assertEqual(doc["totals"]["advisories"], 4)
        self.assertEqual(doc["totals"]["with_advisories"], 3)
        self.assertIn("3 with advisories", text)

    def test_none_known_is_stated(self):
        rc, text, out = self.scan("clean.json")
        self.assertEqual(rc, 0, text)
        doc = json.loads(out.read_text())
        self.assertEqual(doc["status"], "none known")
        self.assertTrue(all(c["status"] == "none known" and c["count"] == 0 and c["advisories"] == []
                            for c in doc["components"]))
        page = collect.render_docs(doc, "2026-11-21")
        self.assertIn("**None known**", page)
        self.assertNotIn("This data is", page)

    def test_output_is_deterministic(self):
        _, _, a = self.scan()
        first = a.read_text()
        a.unlink()
        _, _, a = self.scan()
        self.assertEqual(first, a.read_text())

    def test_scan_failure_is_exit_2_and_writes_nothing(self):
        fx = self.tmp / "broken.json"
        fx.write_text(json.dumps({"pins": {}, "resolve": {}, "commits": {}, "vulns": {}}))
        out = self.tmp / "v.json"
        rc, text = run(scan, "--root", ROOT, "--offline-fixture", fx, "--out", out)
        self.assertEqual(rc, 2)
        self.assertIn("FAILED", text)
        self.assertFalse(out.exists())

    def test_unknown_advisory_in_fixture_fails(self):
        fx = json.loads((FIX / "osv.json").read_text())
        fx["vulns"].pop("CVE-2099-0001")
        p = self.tmp / "f.json"
        p.write_text(json.dumps(fx))
        rc, _ = run(scan, "--root", ROOT, "--offline-fixture", p, "--out", self.tmp / "v.json")
        self.assertEqual(rc, 2)

    def test_advisory_fields(self):
        _, _, out = self.scan()
        doc = json.loads(out.read_text())
        by = {c["name"]: c for c in doc["components"]}
        a = {x["id"]: x for x in by["threadx"]["advisories"]}
        self.assertEqual(a["CVE-2099-0001"]["severity"], "critical")  # CVSS 3.1 9.8
        self.assertEqual(a["CVE-2099-0001"]["summary"], "Fixture details over two lines.")
        self.assertEqual(a["CVE-2099-0002"]["severity"], "unrated")  # CVSS 4.0 is kept as a vector only
        self.assertTrue(a["CVE-2099-0002"]["cvss"].startswith("CVSS:4.0/"))
        self.assertLessEqual(len(a["CVE-2099-0002"]["summary"]), 160)
        self.assertEqual(a["CVE-2099-0002"]["url"], "https://osv.dev/vulnerability/CVE-2099-0002")
        g = by["driver-ads7952"]["advisories"][0]
        self.assertEqual(g["severity"], "medium")  # GitHub's "moderate"

    def test_cvss3(self):
        self.assertEqual(scan.cvss3_score("CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"), 9.8)
        self.assertEqual(scan.cvss3_score("CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:C/C:H/I:H/A:H"), 10.0)
        self.assertEqual(scan.cvss3_score("CVSS:3.1/AV:L/AC:L/PR:L/UI:N/S:U/C:L/I:N/A:N"), 3.3)
        self.assertEqual(scan.cvss3_score("CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:N/A:N"), 0.0)
        self.assertIsNone(scan.cvss3_score("CVSS:3.1/garbage"))
        self.assertEqual([scan.label(x) for x in (0, 3.9, 4.0, 6.9, 7.0, 8.9, 9.0)],
                         ["none", "low", "medium", "medium", "high", "high", "critical"])


def entry(vid, version=None, status="fixed_in_pinned_version", aliases=()):
    return {"id": vid, "aliases": list(aliases), "component": "threadx", "component_version": version or sbom.THREADX_PIN,
            "status": status, "reason": "The upstream advisory lists the pinned version as patched.",
            "source": "https://github.com/eclipse-threadx/threadx/security/advisories/GHSA-x", "reviewed": "2026-11-21"}


class ReviewedTests(unittest.TestCase):
    def scan(self, entries):
        fx = scan.Fixture(FIX / "osv.json")
        return scan.scan(ROOT, "2026-11-21", fx, fx.resolve, fx.pin, reviewed=entries)

    def threadx(self, doc):
        return next(c for c in doc["components"] if c["name"] == "threadx")

    def test_committed_list_is_valid_and_holds_only_what_was_checked(self):
        entries = scan.load_reviewed(ROOT)
        self.assertEqual(len(entries), 8)
        for e in entries:
            self.assertEqual(e["status"], "fixed_in_pinned_version")
            self.assertEqual(e["component"], "threadx")
            self.assertEqual(e["component_version"], sbom.THREADX_PIN)
            self.assertEqual(len(e["aliases"]), 1)
            self.assertEqual(e["source"], "https://github.com/eclipse-threadx/threadx/security/advisories/" + e["aliases"][0])
            self.assertIn("lists 6.5.2.202603b as patched", e["reason"])
            self.assertNotIn("not built", e["reason"].lower())
        self.assertEqual(len({e["id"] for e in entries}), 8)

    def test_bad_list_is_refused(self):
        with tempfile.TemporaryDirectory() as t:
            root = pathlib.Path(t)
            (root / "catalog" / "schemas").mkdir(parents=True)
            (root / "tools" / "vuln").mkdir(parents=True)
            (root / "catalog" / "schemas" / "vex.schema.json").write_text((ROOT / "catalog/schemas/vex.schema.json").read_text())
            for bad in [{"schema": "kb-vex/1", "entries": [{"id": "x"}]},
                        {"schema": "kb-vex/1", "entries": [{**entry("A"), "status": "not_built"}]},
                        {"schema": "kb-vex/1", "entries": [{**entry("A"), "source": "http://x"}]}]:
                (root / "tools" / "vuln" / "reviewed.json").write_text(json.dumps(bad))
                with self.assertRaises(scan.ScanError):
                    scan.load_reviewed(root)

    def test_review_applies_and_counts_open_and_reviewed(self):
        doc = self.scan([entry("CVE-2099-0001")])
        self.assertEqual(scan.problems(doc), [])
        tx = self.threadx(doc)
        by = {a["id"]: a for a in tx["advisories"]}
        self.assertEqual(by["CVE-2099-0001"]["review"]["status"], "fixed_in_pinned_version")
        self.assertEqual(by["CVE-2099-0001"]["review"]["reviewed"], "2026-11-21")
        self.assertNotIn("review", by["CVE-2099-0002"])
        self.assertEqual((tx["count"], tx["open"], tx["reviewed"], tx["status"]), (2, 1, 1, "advisories open"))
        self.assertEqual((doc["totals"]["advisories"], doc["totals"]["open"], doc["totals"]["reviewed"]), (4, 3, 1))
        self.assertEqual(doc["status"], "advisories open")

    def test_all_reviewed_and_alias_match(self):
        doc = self.scan([entry("OTHER", aliases=["CVE-2099-0001"]), entry("CVE-2099-0002")])
        self.assertEqual(scan.problems(doc), [])
        tx = self.threadx(doc)
        self.assertEqual((tx["open"], tx["reviewed"], tx["status"]), (0, 2, "reviewed: fixed in the pinned version"))
        self.assertEqual(doc["status"], "advisories open")  # the two drivers are still open
        only = scan.Fixture(FIX / "osv.json")
        only.data["commits"] = {only.data["pins"]["third_party/threadx"]: ["CVE-2099-0001", "CVE-2099-0002"]}
        doc = scan.scan(ROOT, "2026-11-21", only, only.resolve, only.pin,
                        reviewed=[entry("CVE-2099-0001"), entry("CVE-2099-0002")])
        self.assertEqual(doc["status"], "reviewed: fixed in the pinned version")
        self.assertEqual(doc["totals"]["open"], 0)

    def test_entry_stops_applying_when_the_pin_moves(self):
        doc = self.scan([entry("CVE-2099-0001", version="v6.5.2.202603_rel")])
        tx = self.threadx(doc)
        self.assertTrue(all("review" not in a for a in tx["advisories"]))
        self.assertEqual((tx["open"], tx["reviewed"], tx["status"]), (2, 0, "advisories open"))
        other = scan.Fixture(FIX / "osv.json")
        doc = scan.scan(ROOT, "2026-11-21", other, other.resolve, other.pin,
                        reviewed=[{**entry("CVE-2099-0001"), "component": "esp-idf"}])
        self.assertTrue(all("review" not in a for a in self.threadx(doc)["advisories"]))

    def test_none_known_only_without_advisories(self):
        fx = scan.Fixture(FIX / "clean.json")
        doc = scan.scan(ROOT, "2026-11-21", fx, fx.resolve, fx.pin, reviewed=[entry("CVE-2099-0001")])
        self.assertEqual(doc["status"], "none known")
        self.assertEqual((doc["totals"]["open"], doc["totals"]["reviewed"]), (0, 0))

    def test_committed_snapshot_has_the_reviews(self):
        doc = json.loads((DATA / "vulnerabilities.json").read_text())
        tx = self.threadx(doc)
        self.assertEqual((tx["count"], tx["open"], tx["reviewed"]), (8, 0, 8))
        self.assertEqual(tx["status"], "reviewed: fixed in the pinned version")
        self.assertEqual((doc["totals"]["open"], doc["totals"]["reviewed"]), (0, 8))

    def test_checks_catch_a_review_that_does_not_add_up(self):
        doc = self.scan([entry("CVE-2099-0001")])
        self.threadx(doc)["open"] = 2
        self.assertTrue(scan.problems(doc))
        doc = self.scan([entry("CVE-2099-0001")])
        self.threadx(doc)["status"] = "none known"
        self.assertTrue(scan.problems(doc))

    def test_page_lists_reviewed_apart_and_never_says_not_built(self):
        doc = json.loads((DATA / "vulnerabilities.json").read_text())
        page = collect.render_docs(doc, doc["checked_at"])
        self.assertIn("**0 open, 8 reviewed**", page)
        self.assertIn("### Reviewed: fixed in the pinned version", page)
        self.assertIn("https://github.com/eclipse-threadx/threadx/security/advisories/GHSA-ffg5-m7vh-vwrp", page)
        self.assertNotIn("not built", page.lower())
        self.assertTrue(all(len(x) <= 120 for x in page.splitlines() if not x.startswith("|")))

    def test_issue_lists_only_open_advisories(self):
        doc = self.scan([entry("CVE-2099-0001")])
        text = issue_body.body(doc)
        self.assertIn("CVE-2099-0002", text)
        self.assertNotIn("- [CVE-2099-0001](https://osv.dev/vulnerability/CVE-2099-0001) (critical)", text)
        self.assertIn("<details>", text)
        self.assertIn("1 reviewed: fixed in the pinned version", text)
        self.assertNotIn("threadx:CVE-2099-0001", issue_body.marker(doc))
        self.assertIn("threadx:CVE-2099-0002", issue_body.marker(doc))
        committed = json.loads((DATA / "vulnerabilities.json").read_text())
        self.assertEqual(issue_body.ids(committed), [])


class CheckTests(unittest.TestCase):
    def good(self):
        return json.loads((DATA / "vulnerabilities.json").read_text())

    def test_committed_snapshot_is_valid(self):
        self.assertEqual(scan.problems(self.good()), [])
        rc, text = run(scan, "--root", ROOT, "--check")
        self.assertEqual(rc, 0, text)

    def test_committed_outputs_are_current(self):
        rc, text = run(collect, "--site", DATA, "--docs", ROOT / "docs" / "assurance" / "vulnerabilities.md", "--check")
        self.assertEqual(rc, 0, text)

    def test_committed_components_are_the_pins(self):
        names = {c["name"] for c in self.good()["components"]}
        compat = json.loads((ROOT / "catalog" / "compat.json").read_text())
        self.assertTrue({d["repo"].split("/")[-1] for d in compat["drivers"]} <= names)
        self.assertTrue({"threadx", "esp-idf"} <= names)

    def test_schema_violations(self):
        for mutate, needle in [
            (lambda d: d.update(schema="kb-vulns/2"), "schema"),
            (lambda d: d.pop("checked_at"), "checked_at"),
            (lambda d: d.update(source="NVD"), "source"),
            (lambda d: d["components"][0].update(commit="abc"), "commit"),
            (lambda d: d["components"][0].update(extra=1), "extra"),
            (lambda d: d.update(stale_after_days=30), "stale_after_days"),
        ]:
            d = self.good()
            mutate(d)
            errs = scan.problems(d)
            self.assertTrue(any(needle in e for e in errs), (needle, errs))

    def test_sum_violations(self):
        d = self.good()
        d["components"][0]["count"] = 3
        self.assertTrue(scan.problems(d))
        d = self.good()
        d["totals"]["advisories"] += 1
        self.assertTrue(scan.problems(d))
        d = self.good()
        d["status"] = "none known" if d["status"] != "none known" else "advisories open"
        self.assertTrue(scan.problems(d))
        d = self.good()
        d["checked_at"] = "2026-13-45"
        self.assertTrue(scan.problems(d))

    def test_check_flags_a_bad_file(self):
        with tempfile.TemporaryDirectory() as t:
            bad = pathlib.Path(t) / "v.json"
            d = self.good()
            d["components"][0]["count"] = 9
            bad.write_text(json.dumps(d))
            rc, _ = run(scan, "--root", ROOT, "--check", bad)
            self.assertEqual(rc, 1)


class CollectTests(unittest.TestCase):
    def test_js_is_the_json(self):
        doc = json.loads((DATA / "vulnerabilities.json").read_text())
        js = collect.render_js(doc)
        self.assertTrue(js.startswith("// SPDX-License-Identifier: MIT\n"))
        body = js.split("globalThis.KB_VULNERABILITIES = ", 1)[1].rstrip().rstrip(";")
        self.assertEqual(json.loads(body), doc)

    def test_stale_text_after_ten_days(self):
        doc = json.loads((DATA / "vulnerabilities.json").read_text())
        day = doc["checked_at"]
        d = collect.datetime.date.fromisoformat(day)
        fresh = (d + collect.datetime.timedelta(days=10)).isoformat()
        old = (d + collect.datetime.timedelta(days=11)).isoformat()
        self.assertNotIn("This data is", collect.render_docs(doc, fresh))
        page = collect.render_docs(doc, old)
        self.assertIn("This data is 11 days old", page)
        self.assertTrue(all(len(x) <= 120 for x in page.splitlines() if not x.startswith("|")))

    def test_check_detects_stale_outputs(self):
        with tempfile.TemporaryDirectory() as t:
            site = pathlib.Path(t)
            (site / "vulnerabilities.json").write_text((DATA / "vulnerabilities.json").read_text())
            page = site / "page.md"
            self.assertEqual(run(collect, "--site", site, "--docs", page)[0], 0)
            self.assertEqual(run(collect, "--site", site, "--docs", page, "--check")[0], 0)
            (site / "vulnerabilities.js").write_text("// edited\n")
            self.assertEqual(run(collect, "--site", site, "--docs", page, "--check")[0], 1)

    def test_invalid_json_is_refused(self):
        with tempfile.TemporaryDirectory() as t:
            site = pathlib.Path(t)
            (site / "vulnerabilities.json").write_text("{}")
            self.assertEqual(run(collect, "--site", site)[0], 1)


class IssueTests(unittest.TestCase):
    def doc(self):
        with tempfile.TemporaryDirectory() as t:
            out = pathlib.Path(t) / "v.json"
            run(scan, "--root", ROOT, "--offline-fixture", FIX / "osv.json", "--out", out, "--date", "2026-11-21")
            return json.loads(out.read_text())

    def test_body_lists_advisories_and_marker(self):
        doc = self.doc()
        text = issue_body.body(doc)
        self.assertIn("CVE-2099-0001", text)
        self.assertIn("### threadx", text)
        found = io.StringIO()
        with contextlib.redirect_stdout(found):
            issue_body.main(["x", "--marker", text])
        self.assertEqual(found.getvalue().strip(), issue_body.marker(doc))

    def test_marker_ignores_everything_but_the_ids(self):
        doc = self.doc()
        before = issue_body.marker(doc)
        doc["checked_at"] = "2027-01-01"
        doc["components"][0]["advisories"][0]["summary"] = "reworded"
        self.assertEqual(issue_body.marker(doc), before)
        doc["components"][-1]["advisories"].append({"id": "CVE-2099-9", "severity": "low", "summary": "s",
                                                    "url": "https://osv.dev/x"})
        self.assertNotEqual(issue_body.marker(doc), before)


if __name__ == "__main__":
    unittest.main()
