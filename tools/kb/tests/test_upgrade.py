# The upgrade planner: every shared upgrade vector, the command line (kb outdated, kb upgrade), a seeded property
# test, and agreement with configurator/resolver.mjs on mutated inputs (ADR-0014).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Tests of kb.upgrade and its commands."""

import contextlib
import copy
import io
import json
import os
import random
import shutil
import subprocess
import tempfile
import unittest

import yaml

from kb import cli, emit, resolver, upgrade

from . import common, minischema
from .test_fuzz import mutate

UPGRADE_VECTORS = os.path.join(common.VECTORS, "upgrade")


def vectors():
    out = []
    for name in sorted(n for n in os.listdir(UPGRADE_VECTORS) if n.endswith(".json")):
        vec = common.load_json(os.path.join(UPGRADE_VECTORS, name))
        cat = vec["catalog"]
        if isinstance(cat, str):
            cat = common.load_json(os.path.normpath(os.path.join(UPGRADE_VECTORS, cat)))
        out.append((name, vec, cat))
    return out


def _pick(entries):
    return [{"code": e["code"], "subject": e["subject"]} for e in entries]


def run_vector(vec, cat):
    opts = vec.get("options", {})
    allow_rc = opts.get("allow_rc", False)
    got = upgrade.plan(cat, vec["image"], vec.get("lock"), opts.get("to"), allow_rc)
    actual = {"ok": got["ok"], "errors": _pick(got["errors"]), "warnings": _pick(got["warnings"]), "rows": got["rows"],
              "image": got["image"], "lock": got["lock"]}
    if got["ok"]:
        actual["files"] = upgrade.files(got, allow_rc)
    return got, actual


class UpgradeVectorTest(unittest.TestCase):
    def test_at_least_twenty_vectors(self):
        self.assertGreaterEqual(len(vectors()), 20)

    def test_every_vector(self):
        for name, vec, cat in vectors():
            with self.subTest(vector=name):
                got, actual = run_vector(vec, cat)
                self.assertEqual(emit.canonical_json(actual), emit.canonical_json(vec["expect"]))
                for e in got["errors"] + got["warnings"]:
                    self.assertTrue(e["code"] in upgrade.CODES or e["code"] in resolver.CODES, e["code"])
                    self.assertTrue(e["message"])

    def test_vectors_cover_every_upgrade_code_and_gate(self):
        seen = set()
        held = set()
        for _, vec, _ in vectors():
            seen |= {e["code"] for e in vec["expect"]["errors"] + vec["expect"]["warnings"]}
            held |= {h["code"] for r in vec["expect"]["rows"] for h in r["held"]}
        self.assertEqual(sorted(set(upgrade.CODES) - seen), [])
        self.assertTrue({"KB-RES-013", "KB-RES-014", "KB-RES-018", "KB-RES-019"} <= held)

    def test_new_images_and_locks_match_the_schemas(self):
        image_schema = common.schema("kb-image.schema.json")
        lock_schema = common.schema("lock.schema.json")
        cat_schema = common.schema("catalog.schema.json")
        for name, vec, cat in vectors():
            self.assertEqual(minischema.errors(cat_schema, cat), [], name)
            if vec["expect"]["ok"]:
                self.assertEqual(minischema.errors(image_schema, vec["expect"]["image"]), [], name)
                self.assertEqual(minischema.errors(lock_schema, vec["expect"]["lock"]), [], name)

    def test_image_text_reads_back_and_resolves_to_the_same_lock(self):
        for name, vec, cat in vectors():
            if not vec["expect"]["ok"]:
                continue
            text = vec["expect"]["files"][upgrade.IMAGE_FILE]
            back = yaml.safe_load(text)
            self.assertEqual(back, vec["expect"]["image"], name)
            self.assertEqual(resolver.resolve(cat, back)["lock"], vec["expect"]["lock"], name)

    def test_an_upgraded_image_is_up_to_date(self):
        """Planning again from the result changes nothing (the planner reaches a fixed point)."""
        for name, vec, cat in vectors():
            if not vec["expect"]["ok"] or vec.get("options", {}).get("to"):
                continue
            allow_rc = vec.get("options", {}).get("allow_rc", False)
            again = upgrade.plan(cat, vec["expect"]["image"], vec["expect"]["lock"], None, allow_rc)
            self.assertTrue(again["ok"], name)
            self.assertEqual([r["change"] for r in again["rows"]], ["none"] * len(again["rows"]), name)

    def test_semver_rules(self):
        self.assertTrue(upgrade._breaking("0.1.0", "0.2.0"))
        self.assertTrue(upgrade._breaking("0.0.1", "0.0.2"))
        self.assertTrue(upgrade._breaking("1.4.0", "2.0.0"))
        self.assertFalse(upgrade._breaking("1.4.0", "1.5.0"))
        self.assertFalse(upgrade._breaking("0.1.0", "0.1.1"))
        self.assertFalse(upgrade._breaking("0.1.0-rc.1", "0.1.0"))
        self.assertEqual(upgrade._change("0.1.0-rc.1", "0.1.0-rc.2"), "prerelease")
        self.assertEqual(upgrade._change("1.0.0", "1.0.1"), "patch")


class UpgradeCliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = self.tmp.name
        vec = common.load_json(os.path.join(UPGRADE_VECTORS, "001-upgrade-with-lock.json"))
        self.vec = vec
        self.catalog = os.path.join(common.VECTORS, "catalogs", "upgrade.json")
        self.image = os.path.join(self.dir, "kb-image.yml")
        with open(self.image, "w", encoding="utf-8") as fh:
            yaml.safe_dump(vec["image"], fh)
        with open(os.path.join(self.dir, emit.LOCK_FILE), "w", encoding="utf-8") as fh:
            fh.write(emit.lock_text(vec["lock"]))

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_outdated_uses_the_lock_beside_the_image(self):
        code, out, err = self.run_cli("outdated", self.image, "--catalog", self.catalog)
        self.assertEqual(code, cli.EXIT_OUTDATED, err)
        self.assertIn("current versions from", err)
        self.assertRegex(out, r"bmp581\s+0\.1\.0\s+0\.2\.1\s+minor\s+yes")
        self.assertIn("1.0.0 (KB-RES-014)", out)

    def test_outdated_json(self):
        code, out, _ = self.run_cli("outdated", self.image, "--catalog", self.catalog, "--json")
        self.assertEqual(code, cli.EXIT_OUTDATED)
        self.assertEqual(json.loads(out)["rows"], self.vec["expect"]["rows"])

    def test_outdated_is_zero_when_up_to_date(self):
        new_dir = os.path.join(self.dir, "new")
        self.assertEqual(self.run_cli("upgrade", self.image, "--catalog", self.catalog, "-o", new_dir)[0], 0)
        code, _, err = self.run_cli("outdated", os.path.join(new_dir, upgrade.IMAGE_FILE), "--catalog", self.catalog)
        self.assertEqual(code, cli.EXIT_OK, err)

    def test_upgrade_writes_the_vector_files(self):
        out_dir = os.path.join(self.dir, "out")
        code, _, err = self.run_cli("upgrade", self.image, "--catalog", self.catalog, "-o", out_dir)
        self.assertEqual(code, 0, err)
        for name, text in self.vec["expect"]["files"].items():
            with open(os.path.join(out_dir, name), encoding="utf-8") as fh:
                self.assertEqual(fh.read(), text, name)
        self.assertTrue(os.access(os.path.join(out_dir, emit.FETCH_FILE), os.X_OK))

    def test_upgrade_without_output_prints_the_report(self):
        code, out, _ = self.run_cli("upgrade", self.image, "--catalog", self.catalog)
        self.assertEqual(code, 0)
        self.assertEqual(out, self.vec["expect"]["files"][upgrade.REPORT_FILE])

    def test_upgrade_to_and_errors(self):
        code, out, err = self.run_cli("upgrade", self.image, "--catalog", self.catalog, "--to", "kb-core=0.3.0",
                                      "--to", "bmp581=0.1.0")
        self.assertEqual(code, 0, err)
        self.assertIn("| kb-core | 0.3.0 | 0.3.0 | none |", out)
        code, _, err = self.run_cli("upgrade", self.image, "--catalog", self.catalog, "--to", "nosuch=1.0.0")
        self.assertEqual(code, cli.EXIT_RESOLVE)
        self.assertIn("error KB-UPG-003 nosuch", err)
        for bad in ("bmp581", "=1.0.0", "bmp581="):
            self.assertEqual(self.run_cli("upgrade", self.image, "--catalog", self.catalog, "--to", bad)[0],
                             cli.EXIT_USAGE)
        code, _, _ = self.run_cli("upgrade", self.image, "--catalog", self.catalog, "--to", "bmp581=0.2.0",
                                  "--to", "bmp581=0.2.1")
        self.assertEqual(code, cli.EXIT_USAGE)

    def test_no_lock_and_explicit_lock(self):
        code, _, err = self.run_cli("outdated", self.image, "--catalog", self.catalog, "--no-lock")
        self.assertEqual(code, cli.EXIT_RESOLVE)  # the unversioned image's defaults do not resolve (vector 013)
        self.assertIn("KB-UPG-001", err)
        other = os.path.join(self.dir, "other.lock.yml")
        shutil.copy(os.path.join(self.dir, emit.LOCK_FILE), other)
        code, _, err = self.run_cli("outdated", self.image, "--catalog", self.catalog, "--lock", other)
        self.assertEqual(code, cli.EXIT_OUTDATED, err)
        self.assertNotIn("current versions from", err)

    def test_lock_reads_without_pyyaml(self):
        from kb import yamlio
        saved = yamlio.yaml
        yamlio.yaml = None
        try:
            self.assertEqual(yamlio.load(os.path.join(self.dir, emit.LOCK_FILE)), self.vec["lock"])
            with self.assertRaises(yamlio.InputError):
                yamlio.load(self.image)
        finally:
            yamlio.yaml = saved


YAML_SAMPLES = [
    "schema: 1\nimage: cli-board\nkb_core: {version: 0.4.0}\nmcu: {family: esp32, target: esp32c6}\n"
    "drivers: [pcal9555, {name: tmc5160, version: 0.1.0-rc.1}]\nbuses:\n  i2c0: {model: guarded, devices: [pcal9555]}\n"
    "  spi1: {model: owner, devices: [tmc5160]}\n",
    "# hand-written\nschema: 1\nkb_core:\n  version: '0.4.0'\nmcu:\n  family: esp32   # the family\ndrivers:\n"
    "  - name: bmp581\n    version: \"0.2.0\"\n  - pf1550\nbuses:\n  i2c0:\n    model: guarded\n    devices:\n"
    "    - bmp581\n"
    "    - pf1550\n",
    "---\nschema: 1\nimage: x\nrtos: ~\nflags: [yes, no, on, off, true, null]\nnums: [1, -2, 0.5, 1.0, 10_000]\n"
    "text: 'it''s # not a comment'\nkb_core: {}\nmcu: {family: host}\n",
    "drivers:\n- a\n- {name: b}\n- [c, d]\nempty: []\nnone: {}\nkey with spaces: plain value\n",
    "a: -.5\nb: .5\nc: 1.\nd: 1_000\ne: \"2026-11-14\"\nf: \"x\"# c\ng: 1.0e5\nh: +7\ni: 0\n",
]

# Plain forms PyYAML resolves to octal, hexadecimal, binary, base-60, infinity, dates or non-string keys: the site
# refuses them ("quote it") and kb's resolver refuses the non-string values PyYAML gives, so neither accepts them.
YAML_REFUSED = [
    "image: 010\n", "image: 0x1F\n", "image: 0b101\n", "image: 1:30\n", "image: .inf\n", "image: 2026-11-14\n",
    "1: a\n", "buses: {1: x}\n",
]


class ParserParityTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_site_yaml_reader_matches_pyyaml(self):
        """configurator/js/project.js reads kb-image.yml documents as PyYAML (kb upgrade) does."""
        docs = list(YAML_SAMPLES)
        for _, vec, _ in vectors():
            if vec["expect"]["ok"]:
                docs.append(vec["expect"]["files"][upgrade.IMAGE_FILE])
        conf = os.path.join(common.ROOT, "configurator", "js")
        script = (
            "const vm = require('vm'); const fs = require('fs'); const path = require('path');\n"
            "const ctx = vm.createContext({}); ctx.globalThis = ctx;\n"
            "for (const f of ['core.js', 'project.js'])\n"
            "  vm.runInContext(fs.readFileSync(path.join(process.argv[2], f), 'utf8'), ctx);\n"
            "const docs = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));\n"
            "process.stdout.write(JSON.stringify(docs.map((d) => {\n"
            "  try { return { ok: ctx.KB.project.parseYaml(d) }; } catch (e) { return { error: String(e.message) }; }\n"
            "})));\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "docs.json"), "w", encoding="utf-8") as fh:
                json.dump(docs + YAML_REFUSED, fh)
            with open(os.path.join(tmp, "run.cjs"), "w", encoding="utf-8") as fh:
                fh.write(script)
            res = subprocess.run(["node", os.path.join(tmp, "run.cjs"), conf, os.path.join(tmp, "docs.json")],
                                 capture_output=True, text=True, check=False)
        self.assertEqual(res.returncode, 0, res.stderr)
        got = json.loads(res.stdout)
        for doc, js in zip(docs, got):
            self.assertIn("ok", js, f"{doc!r}: {js}")
            self.assertEqual(json.loads(json.dumps(yaml.safe_load(doc))), js["ok"], doc)
        for doc, js in zip(YAML_REFUSED, got[len(docs):]):
            self.assertIn("error", js, doc)
            loaded = yaml.safe_load(doc)
            values = list(loaded.keys()) + [v for v in loaded.values() if not isinstance(v, dict)]
            values += [k for v in loaded.values() if isinstance(v, dict) for k in v]
            self.assertTrue(any(not isinstance(v, str) for v in values), doc)  # kb refuses it as KB-RES-001


class UpgradeFuzzTest(unittest.TestCase):
    ROUNDS = 1500

    def cases(self, seed, rounds):
        rng = random.Random(seed)
        seeds = [(vec, cat) for _, vec, cat in vectors()]
        out = []
        for _ in range(rounds):
            vec, cat = rng.choice(seeds)
            image = vec["image"]
            lock = vec.get("lock")
            to = vec.get("options", {}).get("to")
            roll = rng.random()
            if roll < 0.4:
                image = mutate(rng, image)
            elif roll < 0.7 and lock is not None:
                lock = mutate(rng, lock)
            elif roll < 0.85:
                to = mutate(rng, to or {"kb-core": "0.4.1"})
            else:
                lock = copy.deepcopy(vec["expect"]["lock"]) if vec["expect"]["ok"] else lock
            out.append((vec, cat, image, lock, to, rng.random() < 0.3))
        return out

    def test_mutated_inputs_give_structured_results(self):
        ok = 0
        for _, cat, image, lock, to, allow_rc in self.cases(77, self.ROUNDS):
            r = upgrade.plan(cat, image, lock, to, allow_rc)
            self.assertEqual(set(r), {"ok", "errors", "warnings", "rows", "image", "lock"})
            for e in r["errors"] + r["warnings"]:
                self.assertTrue(e["code"] in upgrade.CODES or e["code"] in resolver.CODES, e)
                self.assertIsInstance(e["subject"], str)
            if r["ok"]:
                ok += 1
                upgrade.files(r, allow_rc)
            else:
                self.assertTrue(r["errors"])
        self.assertGreater(ok, 0)

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_javascript_agrees_on_mutated_inputs(self):
        cases = self.cases(1515, 800)
        payload = [{"catalog": cat, "image": image, "lock": lock, "to": to, "allow_rc": allow_rc}
                   for _, cat, image, lock, to, allow_rc in cases]
        script = (
            "import { readFileSync } from 'node:fs';\n"
            "import { plan, upgradeFiles } from %s;\n"
            "const out = JSON.parse(readFileSync(process.argv[2], 'utf8')).map((c) => {\n"
            "  const r = plan(c.catalog, c.image, { lock: c.lock, to: c.to, allowRc: c.allow_rc });\n"
            "  const pick = (l) => l.map((e) => [e.code, e.subject]);\n"
            "  return { ok: r.ok, errors: pick(r.errors), warnings: pick(r.warnings), rows: r.rows,\n"
            "           files: r.ok ? upgradeFiles(r, c.allow_rc) : null };\n"
            "});\n"
            "process.stdout.write(JSON.stringify(out));\n"
        ) % json.dumps("file://" + os.path.join(common.ROOT, "configurator", "resolver.mjs"))
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "cases.json"), "w", encoding="utf-8") as fh:
                json.dump(payload, fh)
            with open(os.path.join(tmp, "run.mjs"), "w", encoding="utf-8") as fh:
                fh.write(script)
            res = subprocess.run(["node", os.path.join(tmp, "run.mjs"), os.path.join(tmp, "cases.json")],
                                 capture_output=True, text=True, check=False)
        self.assertEqual(res.returncode, 0, res.stderr)
        for case, js in zip(payload, json.loads(res.stdout)):
            r = upgrade.plan(case["catalog"], case["image"], case["lock"], case["to"], case["allow_rc"])
            py = {"ok": r["ok"], "errors": [[e["code"], e["subject"]] for e in r["errors"]],
                  "warnings": [[e["code"], e["subject"]] for e in r["warnings"]], "rows": r["rows"],
                  "files": upgrade.files(r, case["allow_rc"]) if r["ok"] else None}
            self.assertEqual(py, js, json.dumps({k: case[k] for k in ("image", "lock", "to")})[:2000])


if __name__ == "__main__":
    unittest.main()


class SiteCatalogueTest(unittest.TestCase):
    """The site and `kb upgrade --catalog configurator/data/catalog.json` read the same catalogue and plan alike."""

    SITE = os.path.join(common.ROOT, "configurator", "data", "catalog.json")

    def node_json(self, script, *args):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "run.mjs")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(script)
            res = subprocess.run(["node", path, *args], capture_output=True, text=True, check=False)
        self.assertEqual(res.returncode, 0, res.stderr)
        return json.loads(res.stdout)

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_same_derived_catalogue_and_plans_as_the_site(self):
        from kb import catalog
        site = common.load_json(self.SITE)
        py_cat = catalog.from_site(site)
        script = (
            "import vm from 'node:vm'; import { readFileSync } from 'node:fs'; import { join } from 'node:path';\n"
            "import { plan, upgradeFiles } from %s;\n"
            "const [conf, sitePath] = process.argv.slice(2);\n"
            "const ctx = vm.createContext({}); ctx.globalThis = ctx;\n"
            "vm.runInContext(readFileSync(join(conf, 'js', 'shared.js'), 'utf8'), ctx);\n"
            "const cat = JSON.parse(JSON.stringify(ctx.KB.shared.kbCatalog(JSON.parse(readFileSync(sitePath, 'utf8')))));\n"
            "const plans = cat.drivers.filter((d) => d.versions.length).map((d) => {\n"
            "  const img = { schema: 1, image: 'probe', kb_core: { version: cat.kb_core.versions[0].version },\n"
            "    mcu: { family: d.versions[0].mcus.includes('any') ? 'host' : d.versions[0].mcus[0] },\n"
            "    drivers: [{ name: d.name, version: d.versions[d.versions.length - 1].version }] };\n"
            "  const r = plan(cat, img, { allowRc: true });\n"
            "  return { image: img, ok: r.ok, codes: r.errors.concat(r.warnings).map((e) => e.code + ' ' + e.subject),\n"
            "           files: r.ok ? upgradeFiles(r, true) : null };\n"
            "});\n"
            "process.stdout.write(JSON.stringify({ cat, plans }));\n"
        ) % json.dumps("file://" + os.path.join(common.ROOT, "configurator", "resolver.mjs"))
        js = self.node_json(script, os.path.join(common.ROOT, "configurator"), self.SITE)
        self.assertEqual(emit.canonical_json(py_cat), emit.canonical_json(js["cat"]))
        self.assertGreater(len(js["plans"]), 10)
        for p in js["plans"]:
            r = upgrade.plan(py_cat, p["image"], None, None, True)
            self.assertEqual(r["ok"], p["ok"], p["image"])
            self.assertEqual([f"{e['code']} {e['subject']}" for e in r["errors"] + r["warnings"]], p["codes"])
            if r["ok"]:
                self.assertEqual(upgrade.files(r, True), p["files"])

    def test_cli_accepts_the_site_catalogue_and_refuses_others(self):
        with tempfile.TemporaryDirectory() as tmp:
            site = common.load_json(self.SITE)
            d = next(x for x in site["drivers"] if x["versions"] and "any" in x["versions"][0]["mcus"])
            image = os.path.join(tmp, "kb-image.json")
            with open(image, "w", encoding="utf-8") as fh:
                json.dump({"schema": 1, "kb_core": {"version": site["kb_core"]["releases"][0]["version"]},
                           "mcu": {"family": "host"}, "drivers": [{"name": d["name"],
                                                                   "version": d["versions"][0]["version"]}]}, fh)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(["outdated", image, "--catalog", self.SITE, "--allow-rc"])
            self.assertIn(code, (cli.EXIT_OK, cli.EXIT_OUTDATED), err.getvalue())
            bad = os.path.join(tmp, "bad.json")
            for doc in ({"schema": 1, "drivers": [{"name": "x"}]}, [1, 2], {"families": [], "kb_core": {"releases": 3}}):
                with open(bad, "w", encoding="utf-8") as fh:
                    json.dump(doc, fh)
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as e:
                    self.assertEqual(cli.main(["outdated", image, "--catalog", bad]), cli.EXIT_USAGE, doc)
                    self.assertEqual(cli.main(["resolve", image, "--catalog", bad]), cli.EXIT_USAGE, doc)
                self.assertIn("catalogue", e.getvalue())
