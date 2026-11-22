# Port SDK

A port (the code that gives kb-core's contracts to one MCU family) needs a small part of kb-core: the public headers
of the interfaces and `kb::dma`, the host models, the conformance suites and the CMake helpers. Every release
publishes exactly that as one file, so a port pins a version and a hash instead of keeping a copy of kb-core.

## The release files

| File | What |
| --- | --- |
| `kb-port-sdk-<version>.tar.gz` | The SDK: one folder `kb-port-sdk-<version>/` |
| `kb-port-sdk-<version>.manifest.json` | The version, the commit, the layout, the suites, the SHA-256 of every file in the SDK, and the SHA-256 of the archive |

Both are listed in `SHA256SUMS`, described in the SPDX SBOM (`kb-core-<version>.spdx.json`) and covered by the same
signed build provenance as every other release file:

```bash
gh release download v0.1.0-rc.2 -R kokeb-labs/kb-core -p 'kb-port-sdk-*' -p SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify kb-port-sdk-0.1.0-rc.2.tar.gz -o kokeb-labs
```

Release candidates (`-rc.N`) are pre-releases with the same files, so a port can pin one while it develops against
new kb-core APIs.

## What is inside

The file list is `vendor` in `tools/layout/paths.yml`, the list a port copies today (`python3 tools/layout/layout.py
vendor`), plus a slice of the catalogue:

| Content | Where |
| --- | --- |
| Public headers of `kb::utils`, `kb::rtos` (none and FreeRTOS backends), `kb::dma`, `kb::interfaces` | `components/lib/<part>/inc` |
| Host models and fakes of the interfaces and the DMA hardware side | `components/lib/dma/host`, `components/lib/interfaces/host` |
| The DMA conformance suite T1 to T18 | `components/lib/dma/host/inc/kb/dma/host/conformance.hpp` |
| The host port and the test harness | `components/ports/host`, `tests/harness` |
| CMake helpers and the layout file they read | `CMakeLists.txt`, `cmake/`, `tools/layout/paths.yml`, `kb.yml` |
| The kb-core version and the ports, without the driver list | `catalog/compat.yml` (a slice) |
| The licence | `LICENSE` |
| The per-file hashes | `SDK-MANIFEST.json` (the same content as the release manifest, without the archive hash) |

Nothing else: no driver, no handler, no documentation, no unit tests of kb-core.

## Using it

Extract the archive into the build tree and add it as a subdirectory, as a port does with a copy today:

```cmake
set(KB_PARTS utils rtos dma interfaces CACHE STRING "")
set(KB_QUIET ON)
add_subdirectory("${KB_SDK_DIR}" "${CMAKE_BINARY_DIR}/kb-core" EXCLUDE_FROM_ALL SYSTEM)
target_link_libraries(my_port PRIVATE kb::interfaces kb::dma)   # tests: kb::interfaces_host, kb::dma_host
```

`tests/port_sdk/minimal_port/` is a complete example: one pin class that satisfies the GPIO contract, and the DMA
conformance suite run against the host model. The test `kb_port_sdk` builds the archive from the checkout, extracts
it into an empty folder and builds and runs that port, so the SDK cannot lose a file a port needs without CI saying so.

## Checking an archive

```bash
python3 tools/release/port_sdk.py verify kb-port-sdk-<version>.tar.gz --manifest kb-port-sdk-<version>.manifest.json
```

It refuses an archive with an unsafe member name, a member that is not a regular file, a file that differs from the
manifest inside the archive, a file the manifest does not list, an archive whose SHA-256 differs from the release
manifest, or a release manifest that disagrees with the one inside.

## How it is built

`tools/release/port_sdk.py build --tag vX.Y.Z-rc.N --out DIR` reads the tracked files under the `vendor` entries and
writes a reproducible archive (sorted members, owner 0, time 0, gzip without a name or time), so the same commit gives
the same bytes. It refuses a tag whose `X.Y.Z` differs from `cmake/kb_version.cmake`: the version a port reads in
the archive name and the one CMake reports are the same.

The release is split by privilege, so no code from the tagged tree runs with write access:

1. On a pushed `v*` tag the CI workflow runs `ci-port-sdk.yml` with read-only permissions. It builds the SDK from the
   tag, verifies it (`port_sdk.py verify`) and uploads it as the artifact `kb-port-sdk`.
2. When that CI run succeeds, `release.yml` (privileged: `contents: write`, `actions: read`) downloads the artifact
   of the triggering run by its run id. It checks out only the default branch (for the tag's history, nothing of it is
   run), confirms the tag points at the run's commit, and checks the artifact: exactly the archive and the manifest,
   the archive's SHA-256 equal to the manifest's, the manifest's version equal to the tag and its commit equal to the
   released commit. Then the pinned ci-tools asset script attaches both files as extra release files (a line in
   `SHA256SUMS`, an entry in the SPDX SBOM) and the attestation job covers them with the others.

## What a port does with it

A port keeps a small pin file (`version`, `sdk_sha256`, `release_url`) and a script that downloads the archive into the
build tree, checks the hash against the pin and, in CI, the attestation. The port's own release states which kb-core
version and which SDK hash it was built against. Those parts belong to the ports, not to kb-core.
