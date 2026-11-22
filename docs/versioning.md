# Versioning and release

- **One version** for all of kb-core, set only in `cmake/kb_version.cmake` and carried by every part's package
  (`find_package(kb 0.1 COMPONENTS utils rtos)`). Semantic Versioning over the union of the parts: a breaking
  change in any part is a major (before 1.0: a minor) for all. While `components/ports/esp32/` lives here it is released
  with
  kb-core; after it moves out it has its own version ([ADR-0007](adr/ADR-0007-mcu-ports-isolated.md)).
- `CHANGELOG.md` keeps one `[Unreleased]` with a section per part, one for the ESP32 port and one for driver pins.
- **Driver pins** ([ADR-0002](adr/ADR-0002-driver-consumption.md), [ADR-0012](adr/ADR-0012-pins-and-release-lock.md)):
  every pin is the commit of a `vX.Y.Z` or `vX.Y.Z-rc.N` tag whose CI passed, never a branch. A bump is one pull
  request with the gitlink, the `components/drivers/drivers.lock.yml` entry, binding and test changes, the regenerated
  `catalog/compat.yml` and a CHANGELOG line.
- **Release**: tag `vX.Y.Z` (release candidates `vX.Y.Z-rc.N` first) on a green `main`. The release carries one
  package per part, `SHA256SUMS`, `compat.yml`, signed build provenance and the release evidence:

  | File | What |
  | --- | --- |
  | `kb-port-sdk-<version>.tar.gz`, `kb-port-sdk-<version>.manifest.json` | What a port needs from kb-core, as one file ([Port SDK](port-sdk.md)) |
  | `kb-core-<version>.lock.yml` | kb-core's own tag and commit SHA, every pin (tag, commit, release or rc), the toolchain and SOUP versions CI used, the SHA-256 of every release file |
  | `kb-core-<version>.spdx.json`, `kb-core-<version>.cdx.json` | SBOM in SPDX 2.3 and CycloneDX 1.6 |
  | `kb-core-<version>.cve.json` | Known-CVE scan of the SBOM, with scan date and database version (re-scanned on a schedule) |
  | `kb-core-<version>.licences.md` | Licence report: each component's SPDX licence and copyright holder |

- A driver states the oldest kb-core it needs in its `driver.yml` (`kb_core_min`); a pin whose `kb_core_min` is
  above the kb-core being built fails CI.
- Later: tested version sets `kb YYYY.N`, the `kb` tool with `kb-image.yml`, and the hosted configurator, all
  reading the catalogue, the release lock and the evidence ([Configurator](design/configurator.md)).
