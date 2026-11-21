# Robustness: random and mutated kb-image documents only ever produce structured results, never an exception.
# Seeded, so every run checks the same documents.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Property test of kb.resolver over malformed images."""

import copy
import json
import os
import random
import shutil
import subprocess
import tempfile
import unittest

from kb import emit, resolver

from . import common

ATOMS = [None, True, False, 0, 1, -1, 1.5, "", "x", "0.4.0", "0.1.0-rc.1", "esp32", "host", "owner", "guarded",
         "pcal9555", "bno08x", "ws2812_rmt", "i2c0", "freertos", "threadx", "é", [], {}]
KEYS = ["schema", "image", "kb_core", "mcu", "rtos", "drivers", "buses", "version", "family", "target", "port",
        "name", "model", "devices", "kind", "i2c0", "spi1", "x"]


def random_value(rng, depth=0):
    roll = rng.random()
    if depth > 3 or roll < 0.45:
        return copy.deepcopy(rng.choice(ATOMS))
    if roll < 0.7:
        return [random_value(rng, depth + 1) for _ in range(rng.randrange(4))]
    return {rng.choice(KEYS): random_value(rng, depth + 1) for _ in range(rng.randrange(5))}


def mutate(rng, doc):
    """Replace, delete or add one value somewhere in ``doc``."""
    doc = copy.deepcopy(doc)
    node = doc
    for _ in range(rng.randrange(4)):
        children = list(node.items()) if isinstance(node, dict) else list(enumerate(node)) if isinstance(node, list) else []
        containers = [(k, v) for k, v in children if isinstance(v, (dict, list)) and v]
        if not containers:
            break
        _, node = rng.choice(containers)
    if isinstance(node, dict):
        op = rng.randrange(3)
        if op == 0 and node:
            del node[rng.choice(list(node))]
        else:
            node[rng.choice(KEYS) if op == 1 or not node else rng.choice(list(node))] = random_value(rng)
    elif isinstance(node, list):
        if node and rng.random() < 0.5:
            node[rng.randrange(len(node))] = random_value(rng)
        else:
            node.append(random_value(rng))
    return doc


class FuzzTest(unittest.TestCase):
    ROUNDS = 3000

    def check(self, cat, image):
        r = resolver.resolve(cat, image)
        self.assertEqual(set(r), {"ok", "errors", "warnings", "lock"})
        for e in r["errors"] + r["warnings"]:
            self.assertIn(e["code"], resolver.CODES)
            self.assertIsInstance(e["subject"], str)
        if r["ok"]:
            self.assertEqual(r["errors"], [])
            emit.files(r["lock"])
        else:
            self.assertIsNone(r["lock"])
            self.assertTrue(r["errors"])
        return r

    def test_random_documents(self):
        rng = random.Random(14)
        cat = common.base_catalog()
        for _ in range(self.ROUNDS):
            self.check(cat, random_value(rng))

    def test_mutated_vectors(self):
        rng = random.Random(2026)
        seeds = [(vec["image"], cat) for _, vec, cat in common.vectors() if cat.get("schema") == 1]
        ok = 0
        for _ in range(self.ROUNDS):
            image, cat = rng.choice(seeds)
            ok += self.check(cat, mutate(rng, image))["ok"]
        self.assertGreater(ok, 0)


    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_javascript_agrees_on_mutated_vectors(self):
        """The same mutated images through configurator/resolver.mjs give the same codes, subjects and locks."""
        rng = random.Random(1414)
        seeds = [(vec["image"], vec["catalog"]) for _, vec, cat in common.vectors() if cat.get("schema") == 1]
        cases = []
        for _ in range(1500):
            image, cat_ref = rng.choice(seeds)
            cases.append({"catalog": cat_ref, "image": mutate(rng, image)})
        script = (
            "import { readFileSync } from 'node:fs';\n"
            "import { join } from 'node:path';\n"
            "import { resolve, files } from %s;\n"
            "const [dir, path] = process.argv.slice(2);\n"
            "const out = JSON.parse(readFileSync(path, 'utf8')).map(({ catalog, image }) => {\n"
            "  const cat = typeof catalog === 'string' ? JSON.parse(readFileSync(join(dir, catalog), 'utf8')) : catalog;\n"
            "  const r = resolve(cat, image);\n"
            "  const pick = (l) => l.map((e) => [e.code, e.subject]);\n"
            "  return { ok: r.ok, errors: pick(r.errors), warnings: pick(r.warnings), files: r.ok ? files(r.lock) : null };\n"
            "});\n"
            "process.stdout.write(JSON.stringify(out));\n"
        ) % json.dumps("file://" + os.path.join(common.ROOT, "configurator", "resolver.mjs"))
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "cases.json"), "w", encoding="utf-8") as fh:
                json.dump(cases, fh)
            with open(os.path.join(tmp, "run.mjs"), "w", encoding="utf-8") as fh:
                fh.write(script)
            res = subprocess.run(["node", os.path.join(tmp, "run.mjs"), common.VECTORS, os.path.join(tmp, "cases.json")],
                                 capture_output=True, text=True, check=False)
        self.assertEqual(res.returncode, 0, res.stderr)
        got = json.loads(res.stdout)
        cats = {}
        for case, js in zip(cases, got):
            ref = case["catalog"]
            if ref not in cats:
                cats[ref] = common.load_json(os.path.join(common.VECTORS, ref))
            r = resolver.resolve(cats[ref], case["image"])
            py = {"ok": r["ok"], "errors": [[e["code"], e["subject"]] for e in r["errors"]],
                  "warnings": [[e["code"], e["subject"]] for e in r["warnings"]],
                  "files": emit.files(r["lock"]) if r["ok"] else None}
            self.assertEqual(py, js, json.dumps(case["image"]))


if __name__ == "__main__":
    unittest.main()
