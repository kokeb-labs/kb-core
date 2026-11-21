# ADR-0001: Layering rule and enforcement

Status: Accepted (owner decision, 2026-11-14). Rule 2 amended by [ADR-0007](ADR-0007-mcu-ports-isolated.md) (MCU
ports in `components/ports/<mcu>/`).

## Context

Layered embedded stacks often state a platform-free rule without checking it. In one earlier stack an audit found
more than twenty places where platform or product code had leaked into handlers and libraries, and none was caught
because every MCU branch compiled out in the host build. Applications could also include interface headers
directly, so the HAL was not the only way to hardware.

## Decision

1. Dependencies point down only: `kb::utils <- kb::rtos <- kb::interfaces <- kb::handlers`. Applications may use
   `kb::utils` and `kb::rtos`; `kb::interfaces` and `kb::handlers` are for the HAL only.
2. SDK and RTOS headers are allowed only in the `.cpp` boundary files listed in a `boundary-files.yml`: kb-core's
   own list (`deviations/boundary-files.yml`, RTOS backends in `components/lib/rtos/backends/<rtos>/`) and each MCU
   port's
   list (`components/ports/<mcu>/deviations/boundary-files.yml`). No `#if` on a platform macro elsewhere.
3. Independent checks, each able to fail CI: CMake visibility (backends PRIVATE; HALs link interfaces and handlers
   PRIVATE; a port sees kb-core only through its installed package), the include and dependency checker
   (`tools/include_checker/`), the poison build (`tests/poison/`) and a symbol lint on linked objects.
4. Board and product values reach kb-core only as configuration.

## Consequences

- A leak is a CI failure, not a review comment. HALs reuse the same checker with their own zone file.
- The few boundary files concentrate every SDK call and every MISRA deviation, so review effort goes there.
- Some convenience is lost: a HAL cannot re-export interface headers to its applications.

## Alternatives considered

- Review only: failed before. CMake visibility only: does not stop include-by-path or a public header leak.
- One target for all of kb-core: simpler to consume, but cannot express "applications may use utils and rtos
  only".
