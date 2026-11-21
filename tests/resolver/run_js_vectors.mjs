// Runs every resolver vector of tests/resolver/vectors through configurator/resolver.mjs and compares the result
// with the vector's expectation: the same vectors tools/kb/tests/test_vectors.py runs through the Python resolver
// (ADR-0014). Usage: node tests/resolver/run_js_vectors.mjs [vectors-dir]. Without a folder it then runs the upgrade
// vectors too (run_js_upgrade_vectors.mjs), so CTest's kb_resolver_js_vectors covers both. Exit 0 when all pass.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { spawnSync } from "node:child_process";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { canonicalJson, files, resolve } from "../../configurator/resolver.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const dir = process.argv[2] || join(here, "vectors");
const pick = (list) => list.map((e) => ({ code: e.code, subject: e.subject }));
let failed = 0;
let count = 0;
for (const name of readdirSync(dir).filter((n) => n.endsWith(".json")).sort()) {
  count += 1;
  const vector = JSON.parse(readFileSync(join(dir, name), "utf8"));
  const catalog = typeof vector.catalog === "string"
    ? JSON.parse(readFileSync(join(dir, vector.catalog), "utf8")) : vector.catalog;
  const got = resolve(catalog, vector.image);
  const exp = vector.expect;
  const actual = { ok: got.ok, errors: pick(got.errors), warnings: pick(got.warnings) };
  if (got.ok) {
    actual.lock = got.lock;
    actual.files = files(got.lock);
  }
  for (const e of [...got.errors, ...got.warnings]) {
    if (typeof e.message !== "string" || e.message.length === 0) actual.errors.push({ code: "NO-MESSAGE", subject: e.code });
  }
  if (canonicalJson(actual) !== canonicalJson(exp)) {
    failed += 1;
    console.log(`FAIL ${name}`);
    console.log(`  expected ${canonicalJson(exp).slice(0, 2000)}`);
    console.log(`  got      ${canonicalJson(actual).slice(0, 2000)}`);
  } else {
    console.log(`ok   ${name}`);
  }
}
if (count === 0) {
  console.log(`no vectors in ${dir}`);
  process.exit(1);
}
console.log(`${count - failed}/${count} vector(s) passed (JavaScript resolver)`);
let upgrades = 0;
if (!process.argv[2]) {
  upgrades = spawnSync(process.execPath, [join(here, "run_js_upgrade_vectors.mjs")], { stdio: "inherit" }).status;
}
process.exit(failed || upgrades ? 1 : 0);
