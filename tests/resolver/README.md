# tests/resolver/

The shared vectors of the resolver specification (ADR-0014, `docs/design/assembler.md`). The Python resolver
(`tools/kb/tests/test_vectors.py`) and the site's resolver (`run_js_vectors.mjs`, Node.js 18 or newer) run every
vector and must agree with its expectation byte for byte.

- `vectors/NNN-<case>.json`: `description`, `catalog` (a path under `vectors/catalogs/` or an inline catalogue),
  `image`, and `expect`: `ok`, the `code` and `subject` of each error and warning, and for a valid image the
  `lock` and every emitted file (`files`).
- `vectors/catalogs/*.json`: small catalogues with made-up commits, shaped like `kb catalog build` output;
  `upgrade.json` has several versions per part, release candidates, `kb_core_min` and bus gates and `osv` scans.
- `vectors/upgrade/NNN-<case>.json`: the upgrade planner (`kb outdated`, `kb upgrade`, the site's upgrade view):
  `description`, `catalog`, `image`, an optional `lock`, `options` (`to`, `allow_rc`) and `expect`: `ok`, codes and
  subjects, `rows`, the new `image` and `lock`, and every emitted file. `tools/kb/tests/test_upgrade.py` and
  `run_js_upgrade_vectors.mjs` (started by `run_js_vectors.mjs` when it is given no folder) run them.

A rule change updates the specification, both resolvers and the affected vectors in one pull request. Review an
expectation by reading it: the vectors are the specification's examples, not snapshots to refresh blindly.
