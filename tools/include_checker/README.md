# Include and dependency checker (specification)

Design only; no implementation yet. Rules it enforces: ADR-0001 and `docs/layering.md`.

## Purpose

Prove on every pull request that dependencies point down only, that applications reach hardware only through
the HAL, and that SDK and RTOS headers appear only in the listed boundary files. CMake visibility alone cannot
prove this (a public include path leaks to every consumer, and a header can be included by path).

## Inputs

- `tools/include_checker/rules.yml`: the zones (path globs) and, for each zone, the zones and system headers it
  may include. HALs and kb-hal-common pass their own zone file that extends this one with `hal_private`,
  `hal_public` and `application` zones.
- `deviations/boundary-files.yml` and every `components/ports/<mcu>/deviations/boundary-files.yml`: the only files
  allowed an
  SDK or RTOS include.
- The compile database (`compile_commands.json`) of the build being checked, for the real include search paths,
  and `cmake --graphviz` output for the target graph.
- The handbook's target library subset (from the `kokeb-labs/ci-tools` checkout) for system headers.

## Checks

| Id | Check |
| --- | --- |
| LAY-001 | Every `#include` in a file of zone A resolves to a file of a zone A may include |
| LAY-002 | An `#include <...>` of an SDK or RTOS header (`esp_*`, `driver/*`, `freertos/*`, `hal/*`, `soc/*`, `stm32*`, `tx_api.h`, `cmsis_os*`) appears only in a file listed in `boundary-files.yml`, and only in a `.cpp` there |
| LAY-003 | No public header (`components/lib/*/inc/**`) includes a port, backend or boundary file |
| LAY-004 | No file outside `components/lib/handlers/` includes a driver header; drivers include nothing from kb-core |
| LAY-005 | In a HAL: no application-zone file includes `kb/interfaces/...` or `kb/handlers/...`, and no HAL public header does either |
| LAY-006 | The CMake target graph follows `utils <- rtos <- interfaces <- handlers`; ports and backends are linked PRIVATE; no application target links `kb::interfaces` or `kb::handlers` |
| LAY-007 | No platform macro test (`#if`/`#ifdef` on `ESP_PLATFORM`, `USE_HAL_DRIVER`, `STM32*`, `configUSE_*`, `tx_*`) outside boundary files and the `components/ports/<mcu>/` and `components/lib/rtos/backends/` folders |
| LAY-008 | Every file in `boundary-files.yml` with `status: present` exists, and none is missing from the list |
| LAY-009 | No product or board name from the denylist in `rules.yml` appears in kb-core (identifiers, macros, comments) |
| LAY-010 | No file under `components/lib/` includes a file under `components/ports/`, and no kb-core CMake file adds an MCU port (`components/ports/host/`, the host port, is built by kb-core) (ADR-0007) |
| LAY-011 | A file under `components/ports/<mcu>/` includes from kb-core only public headers `kb/<part>/...` (never `src/`, a backend or a test file) |
| LAY-012 | No `virtual` keyword anywhere in kb-core or its ports (ADR-0006) |
| SYM-001 | Symbol lint: no linked object of a platform-free part or of an application references an SDK or native RTOS symbol |

## Output

One line per finding: `path:line: LAY-NNN message`, a SARIF file for code scanning, a step summary, exit status
1 on any finding. A suppression is not possible; a rule change is a change to `rules.yml` reviewed like code.

## Tests

`tests/layering/good/` must pass with zero findings; each case in `tests/layering/bad/<case>/` must produce
exactly the rule id in its `expect.txt`. The checker's own tests run before it checks the repository.

## Implementation notes

Python 3, standard library plus PyYAML, run with `python3 -I`; resolves includes through the compile database's
`-I`/`-isystem` paths so a header reached by a relative path is still classified. Planned home: ci-tools
reusable workflow `ru-layering`, so HALs reuse it.
