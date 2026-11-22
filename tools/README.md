# tools/

Repository tools. Each folder holds a specification first; the implementation follows under the same pipeline as
the code (tests in the folder, run in CI). Tools shared with other repositories (HALs, kb-hal-common, the
configurator) are meant to move into `kokeb-labs/ci-tools` once written.

| Folder | What it will do |
| --- | --- |
| [`include_checker/`](include_checker/README.md) | Enforce the layering and boundary rules on includes and CMake links |
| [`catalog/`](catalog/README.md) | Generate `catalog/compat.yml` from the pinned drivers |
| [`pins/`](pins/README.md) | Check every driver pin against its release tag, its CI and its `driver.yml` |
| [`layout/`](layout/README.md) | The repository layout file (`paths.yml`), the root allowlist and the retired-location check (implemented) |
| [`examples/`](examples/README.md) | The example index checks: folders, README table, documentation snippets, catalogue (implemented) |
| [`vuln/`](vuln/README.md) | The known-vulnerability scan of the pinned components (OSV) and its site data (implemented) |
| [`release/`](release/README.md) | The port SDK asset of a release (`port_sdk.py`: build, verify) and its tests (implemented) |
| [`kb/`](kb/README.md) | The `kb` tool (implemented): `kb catalog build`, `kb resolve`, `kb sbom` (ADR-0014) |

`preflight.sh` (every CI check that runs on a workstation, in one command) comes from the driver template when
the first part has source.
