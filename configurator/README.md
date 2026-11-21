# configurator/

The kb configurator (ADR-0013, ADR-0014): a static page where a user picks an MCU family, target and board, an
RTOS, kb-core and port versions, drivers from a searchable catalogue with a version each, the bus of every driver
and the thread model of every bus, and downloads a pinned, repeatable project with its evidence. User guide:
[`docs/configurator.md`](../docs/configurator.md). Checks: [`docs/configurator-checks.md`](../docs/configurator-checks.md).

No server, no account, no cookies, no analytics, no network access at run time; a strict Content Security Policy;
every script and the stylesheet pinned by a Subresource Integrity hash.

## Layout

| Path | What it is |
| --- | --- |
| `index.html` | The page, served over https (GitHub Pages); loads the files below, each with `integrity` |
| `offline.html` | The same page in one file for use from disk (generated: `tools/bundle_offline.py`) |
| `resolver.mjs` | The shared resolver and upgrade planner of ADR-0014 (`docs/design/assembler.md`): the same rules and files as `kb resolve` and `kb upgrade` |
| `js/core.js` | Versions, SHA-256, name-based UUIDs, deterministic JSON and YAML |
| `js/resolver.js` | The site checks (`CFG-*` codes): bus instances, addresses, pins, bindings, tested sets, CVEs |
| `js/shared.js` | Derives kb-core's catalogue and image from the site's, runs both rule sets, merges the findings |
| `js/generate.js` | `kb-image.yml`, CycloneDX 1.6 and SPDX 2.3 SBOMs (SOUP, `kb:osv`), licence and status reports, the ZIP; the lock, CMake files and `kb-fetch.sh` come from `resolver.mjs` |
| `js/state.js` | The selection in the URL hash (catalogue identifiers only) |
| `js/project.js` | *Open an existing project*: reads a picked `kb-image.yml` (YAML subset parser) and lock locally, makes the selection |
| `js/ui.js`, `js/boot.mjs`, `css/app.css` | The page |
| `data/platforms.json` | Input: MCU families, targets with bus counts, boards, RTOS backends, bus kinds, binding facts |
| `data/catalog.json`, `data/catalog.js` | The site catalogue (generated: `tools/build_catalog.py`); `catalog.js` is the same JSON for `file://` |
| `assets/figures/<driver>/` | Chip figures (pinout, outline, block diagram, light and dark) copied from each driver's tag |
| `vendor/fflate-0.8.2/` | The ZIP library, MIT, pinned and integrity-checked (see its README) |
| `schemas-draft/` | Draft schemas of the site catalogue, the selection and the site vectors |
| `tests/` | `node --test` suites and the site vectors; `tools/headless_check.mjs` drives Chrome |

## Commands

```bash
python3 tools/build_catalog.py --online --include-head   # regenerate the catalogue (gh signed in for --online)
python3 tools/sri.py --write                             # after changing any script, the CSS or the catalogue
python3 tools/bundle_offline.py                          # then regenerate offline.html
node --test tests/*.test.mjs                             # unit tests, vectors, static page checks
node tools/headless_check.mjs [--shots DIR]              # the page in headless Chrome, served and from file
node tools/export_matrix.mjs OUT --chrome                # export the rows of tools/export-matrix.json through the page
python3 tools/validate_exports.py OUT                    # SPDX, CycloneDX, schemas, licence report, kb resolve agrees
bash tools/export_build.sh OUT/<row>/project host        # kb-fetch.sh and the build (host, cortex-m or esp-idf)
```

`build_catalog.py --check`, `sri.py --check` and `bundle_offline.py --check` fail when a generated file is stale.
The tests run kb-core's shared vectors (`tests/resolver/vectors/`, and its `upgrade/` folder) through `resolver.mjs`
when that folder exists (or `KB_SHARED_VECTORS=<dir>`).

It moves to the `kb-configurator` repository (ADR-0008) when it needs its own release cadence.
