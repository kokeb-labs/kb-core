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

Schema: `catalog/schemas/vulnerabilities.schema.json` (`kb-vulns/1`). Guide:
[Vulnerability data](../../docs/configurator.md#vulnerability-data).
