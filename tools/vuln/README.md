# tools/vuln/

The known-vulnerability scan of the components this kb-core revision pins: every driver at its pinned tag, the
ThreadX kernel and the ESP-IDF release. Each is looked up in the [OSV](https://osv.dev) database by the commit of its
pin. Standard library only; the weekly workflow is `.github/workflows/ci-vulnerabilities.yml`.

```bash
python3 tools/vuln/scan.py                          # scan (network) and write configurator/data/vulnerabilities.json
python3 tools/vuln/scan.py --check                  # validate the committed file: schema, sums, order (no network)
python3 tools/vuln/scan.py --offline-fixture F --out OUT --date 2026-01-01    # answers from a fixture (tests)
python3 tools/vuln/collect.py --site configurator/data --docs docs/assurance/vulnerabilities.md
python3 tools/vuln/collect.py --site configurator/data --docs docs/assurance/vulnerabilities.md --check
python3 tools/vuln/tests/run.py                     # the tests (ctest: kb_vuln_unit)
```

Exit status of `scan.py`: 0 done (advisories found are reported, not an error), 1 `--check` found a problem, 2 the
scan itself failed (network, an answer it cannot read, a component it cannot resolve); a failed scan writes nothing.
`collect.py` writes `vulnerabilities.js` (the file as a script for the page) and the Markdown page; its `--check`
judges the page as of the scan's own date, so it never depends on the clock and never rescans.

## The reviewed list

`reviewed.json` (`kb-vex/1`, `catalog/schemas/vex.schema.json`) holds advisories that were checked against their
primary source. `scan.py` reads it, adds a `review` to each advisory an entry covers, and counts it as reviewed, not
open; only open advisories open the weekly issue. Fields: `id`, `aliases`, `component`, `component_version`, `status`
(`fixed_in_pinned_version`; `not_affected` is allowed and unused), `reason`, `source` (https URL), `reviewed` (date).

To add an entry, the **review rule** applies: the primary source (for ThreadX the upstream GitHub security advisory,
`gh api repos/eclipse-threadx/threadx/security-advisories/GHSA-...`) must name our exact pinned version as patched.
Check that, then write the reason as plain fact (what the source says, which version is pinned), put the advisory URL
in `source` and today's date in `reviewed`. Do not add inference (for example that code is not compiled) to the data.
`component_version` is the version string the scan prints for the component; an entry stops applying when the pin
moves, and the advisory is open again. Then run `scan.py` and `collect.py` and commit the regenerated files.

Known data issue (nothing filed): OSV's range for the eight ThreadX advisories ends with `last_affected` at the pinned
commit and has no `fixed` event, although the upstream advisories list the pinned release as patched; see
[Known data issues](../../docs/configurator.md#known-data-issues).

Schema: `catalog/schemas/vulnerabilities.schema.json` (`kb-vulns/1`; `status` is `none known`, `advisories open` or
`reviewed: ...`). Guide:
[Vulnerability data](../../docs/configurator.md#vulnerability-data).
