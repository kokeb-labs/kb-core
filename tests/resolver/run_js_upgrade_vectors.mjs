// Runs every upgrade vector of tests/resolver/vectors/upgrade through configurator/resolver.mjs (plan, imageText,
// upgradeReport, upgradeFiles) and compares the result with the vector's expectation: the same vectors
// tools/kb/tests/test_upgrade.py runs through tools/kb/upgrade.py (ADR-0014). Usage:
// node tests/resolver/run_js_upgrade_vectors.mjs [vectors-dir]. Exit 0 when every vector passes.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { canonicalJson, plan, upgradeFiles } from "../../configurator/resolver.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const dir = process.argv[2] || join(here, "vectors", "upgrade");
const pick = (list) => list.map((e) => ({ code: e.code, subject: e.subject }));
let failed = 0;
let count = 0;
for (const name of readdirSync(dir).filter((n) => n.endsWith(".json")).sort()) {
  count += 1;
  const vector = JSON.parse(readFileSync(join(dir, name), "utf8"));
  const catalog = typeof vector.catalog === "string"
    ? JSON.parse(readFileSync(join(dir, vector.catalog), "utf8")) : vector.catalog;
  const opts = vector.options || {};
  const got = plan(catalog, vector.image, { lock: vector.lock, to: opts.to, allowRc: opts.allow_rc === true });
  const actual = { ok: got.ok, errors: pick(got.errors), warnings: pick(got.warnings), rows: got.rows, image: got.image, lock: got.lock };
  if (got.ok) actual.files = upgradeFiles(got, opts.allow_rc === true);
  for (const e of [...got.errors, ...got.warnings]) {
    if (typeof e.message !== "string" || e.message.length === 0) actual.errors.push({ code: "NO-MESSAGE", subject: e.code });
  }
  if (canonicalJson(actual) !== canonicalJson(vector.expect)) {
    failed += 1;
    console.log(`FAIL ${name}`);
    console.log(`  expected ${canonicalJson(vector.expect).slice(0, 2000)}`);
    console.log(`  got      ${canonicalJson(actual).slice(0, 2000)}`);
  } else {
    console.log(`ok   ${name}`);
  }
}
if (count === 0) {
  console.log(`no vectors in ${dir}`);
  process.exit(1);
}
console.log(`${count - failed}/${count} upgrade vector(s) passed (JavaScript planner)`);
process.exit(failed ? 1 : 0);
