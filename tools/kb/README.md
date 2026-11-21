# tools/kb/

The `kb` command line: `kb catalog build`, `kb resolve`, `kb outdated`, `kb upgrade`, `kb sbom` (ADR-0014). Python 3.10
or newer, standard library plus PyYAML for YAML input. Usage and exit codes: [docs/kb-tool.md](../../docs/kb-tool.md);
the rules it implements: [docs/design/assembler.md](../../docs/design/assembler.md).

| File | What |
| --- | --- |
| `kb.py` | Script entry point: `python3 tools/kb/kb.py <command>` |
| `__main__.py` | Module entry point: `cd tools && python3 -m kb <command>` |
| `cli.py` | Arguments, files and exit codes |
| `resolver.py` | The resolver: a pure function of (catalogue, image); same rules as `configurator/resolver.mjs` |
| `upgrade.py` | The upgrade planner of `kb outdated` and `kb upgrade`: same rules and bytes as `plan` in the site's resolver |
| `emit.py` | The lock, `CMakePresets.json`, `kb_image.cmake` and `kb-fetch.sh`, byte-identical to the site's |
| `catalog.py` | The catalogue from `driver.yml` at each tag (git clones or `gh api`); the site's catalogue converted |
| `sbom.py` | CycloneDX 1.6 and SPDX 2.3 JSON from a lock |
| `semver.py` | `vX.Y.Z` and `vX.Y.Z-rc.N` tags and their order |
| `platforms.py` | MCU families, RTOS backends and parts; checked against `cmake/kb_build_settings.cmake` |
| `board.py`, `dmadata.py`, `dmacheck.py`, `dmagen.py`, `boardvectors.py` | `board.yml`; the DMA routing data (`data/dma/`); rules R1 to R15; the generator of `kb board gen`; the board vectors ([Board DMA](../../docs/design/board-dma.md)) |
| `yamlio.py` | YAML or JSON input (a lock reads without PyYAML), with a clear message when PyYAML is missing |
| `tests/` | Unit tests, the shared resolver and upgrade vectors, seeded property tests and Python-JavaScript agreement tests |

Run the tests: `python3 tools/kb/tests/run.py`.
