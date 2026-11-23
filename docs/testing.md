# Testing and CI

## Per part

| Part | Unit tests | Fuzz | Mutation | Other |
| --- | --- | --- | --- | --- |
| utils | Every function, boundaries, `static_assert` tests, compile-fail tests | containers, CRCs, curves | yes | Coverage goal 100 % branch |
| rtos | `none` backend with the fake clock; FreeRTOS and ThreadX boundaries over fake kernels; the threaded suite on std::thread and on the ThreadX Linux port | queue and flag sequences | yes | ThreadSanitizer, ESP32 builds, ThreadX backend and kernel for Cortex-M0+, M4F, M7F |
| interfaces | Concept conformance per port, compile-fail tests, host fault injection | loopback bus scripts | yes | ESP-IDF build of the ESP32 port |
| handlers | Bindings over host ports and chip models; start-up plans stage by stage; both thread models; health transitions | command sequences | yes | Integration matrix, TSan on both facades |
| components/ports/esp32 | Link-seam host tests of each boundary file | - | - | ESP-IDF v5.5 build of its examples |

## Coverage and mutation

- **Coverage:** 100 % of lines and branches of the library code (`components/lib/<part>/inc`, the RTOS backends) is the
  target and the current state. Tests pass their inputs through `kb_test::Rt()` (a volatile copy), so a
  `constexpr` function runs at run time and is measured, instead of being folded by the compiler.
- **Mutation:** level `high`, enforced: every scored mutant is killed. A mutant that cannot change behaviour (an
  initial value every path overwrites, a comparison whose two forms agree at the only value where they differ, a
  compile-time margin) is listed in `docs/assurance/equivalent-mutants.yml` with its reason; never deleted.
- Locally: `tools/preflight.sh --container` runs coverage, sanitizers and fuzzing in the CI image.
- **Hung tests:** every kb-core CTest test has a `TIMEOUT` of `KB_TEST_TIMEOUT` seconds (120 by default, about ten
  times the slowest suite's normal run), and the preflight and container gates pass `ctest --timeout 300` for the
  rest, so a hang fails in minutes instead of holding a runner.

## CI

<!-- figure: ci-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="diagrams/ci-flow.dark.svg">
  <img src="diagrams/ci-flow.svg"
    alt="Flow from the local preflight to a pull request, then always-run and path-gated checks into one
    required gate, with the drivers' own tests never re-run, and a tag producing the release assets that
    a schedule re-scans.">
</picture>
<!-- /figure -->

| Check | Workflow | ci-tools reusable workflow |
| --- | --- | --- |
| Host build, tests, clang-tidy, cppcheck | `ci-host.yml` | `ru-host-ci` |
| Coverage, sanitizers, fuzz, complexity (level `high`) | `ci-quality.yml` | `ru-host-quality` |
| MISRA C++:2023 and KB-CPP20 (level `high`) | `ci-misra.yml` | `ru-misra` |
| Cortex-M cross build | `ci-cross-build.yml` | `ru-cross-build` |
| ESP32 port and its examples | `ci-esp32.yml` | `ru-build` |
| Docs | `ci-docs.yml` | `ru-mkdocs-publish` |
| DCO, org standard, lint, handbook, drift | `ci-repo-checks.yml` | `ru-repo-checks` |
| Each part alone, install consumers | `ci-standalone.yml` | `ru-cmake-matrix` |
| Integration of pinned drivers | `ci-integration.yml` | `ru-cmake-matrix`, `ru-host-ci` |
| Mutation | `ci-mutation.yml` | `ru-mutation` |
| Pin hygiene | `ci-pins.yml` | `ru-pin-hygiene` |
| Layering and poison build | `ci-layering.yml` | `ru-layering` |
| compat.yml | `ci-catalog.yml` | `ru-catalog` |
| Layout allowlist, retired locations, example index | `ci-layout.yml` | - (`tools/layout`, `tools/examples`) |
| Release lock, SBOM, CVE scan, licence report | `release.yml` | `ru-release-evidence` |

**Not re-run for driver content**: unit tests, coverage, mutation, fuzzing, MISRA, clang-tidy, cppcheck,
complexity and handbook checks of the drivers. Each pinned driver passed them in its own CI at its release tag;
pin hygiene proves the pin is that tag ([ADR-0004](adr/ADR-0004-ci-strategy.md)).

### Which change runs which jobs

A push to a pull request branch starts one run (the `pull_request` event). Pushes start CI only on `main` and on
`v*` tags, so the same commit is not built twice. The two long jobs, Mutation (shards of 48 to 86 minutes) and MISRA
(about 30 minutes), are run on a pull request only when it changes something they read. The `Changed files` job
decides, and the `CI gate` counts a job skipped by that decision as passed. If the `Changed files` job itself fails
or is cancelled, the gate fails.

| Changed path in the pull request | Mutation and MISRA | Configurator exports | All other jobs |
| --- | --- | --- | --- |
| `docs/**` (except `docs/assurance/equivalent-mutants.yml`), `*.md`, `CHANGELOG.md`, `mkdocs.yml` | skipped | skipped | run |
| `configurator/**` | skipped | run | run |
| `catalog/**` | skipped | run for `catalog/schemas/**` only | run |
| `tools/kb/**` | skipped | run | run |
| `tools/docs`, `tools/examples`, `tools/release`, `tools/vuln`, `tools/evidence`, `tools/pins`, `tools/catalog`, `tools/include_checker` | skipped | skipped | run |
| `examples/**`, `components/drivers/**`, `components/ports/esp32/**` | skipped | run (`components/**`) | run |
| `components/lib/**`, `components/ports/host/**` | run | run | run |
| `cmake/**`, `CMakeLists.txt`, `kb.yml`, `third_party/**`, `deviations/**` | run | run | run |
| `tools/mutants/**`, `tools/lint/**`, `tools/layout/paths.yml`, `docs/assurance/equivalent-mutants.yml` | run | skipped | run |
| `tests/fakes`, `tests/harness`, `tests/integration`, `tests/poison`, `tests/layering`, `tests/standalone` | run | skipped | run |
| `.github/workflows/ci.yml`, `ci-mutation.yml`, `ci-misra.yml` | run | skipped | run |
| Pull request with the label `full-ci` (add it before the push, then push) | run | run | run |
| Push to `main`, a `v*` tag, a merge queue entry | run | run | run |

"Run" and "skipped" apply to a pull request; every other job runs on every pull request and every push. A schedule
or a manual `workflow_dispatch` run is not available: the trigger policy of ci-tools (docs/triggers.md there) allows
only `push`, `pull_request`, `merge_group` and `workflow_call`. Mutation and MISRA therefore always run on `main`,
which catches anything a filter let through after the merge.
