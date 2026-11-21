# ADR-0007: MCU ports start isolated in `components/ports/<mcu>/`, ready to move to their own repositories

Status: Accepted (owner decision, 2026-11-13). Amends [ADR-0001](ADR-0001-layering.md) rule 2 and the layout of
handbook ADR-0013 (`interfaces/mcu/<family>/`).

## Context

An MCU port implements kb-core's contracts over one vendor SDK. It has a different toolchain, SDK container, CI,
SOUP list and MISRA deviation posture from the platform-free parts, and its release cadence follows the SDK, not
kb-core. Keeping ports inside `components/lib/interfaces/` couples all of that to kb-core: every SDK container in
kb-core's
CI, an SDK bump forcing a kb-core release, every family's deviations in kb-core's records. Moving a port out later
is cheap only if the seam exists from the first commit. While the contracts are still changing, a separate
repository would turn every contract change into two pull requests.

## Decision

1. Each MCU family port lives in **`components/ports/<mcu>/`** in kb-core, starting with `components/ports/esp32/`. It
   is
   built as **its own CMake project** (`project(kb_port_<mcu>)`) that uses kb-core only through the installed
   package (`find_package(kb CONFIG REQUIRED COMPONENTS interfaces rtos utils)`), and under ESP-IDF as its own
   component (`kb_port_esp32`). It never uses a kb-core source path, a private header or a kb-core CMake variable.
2. Everything a separate repository would need is already per port: `README.md`, `CMakeLists.txt`,
   `inc/kb/port/<mcu>/` (namespace `kb::port::<mcu>`, target `kb::port_<mcu>`), `src/` with the boundary files,
   `deviations/` (its own `boundary-files.yml` and `deviations.yml`, ids `DEV-KBP-<MCU>-NNN`), `examples/`, its
   own CI caller and its own SOUP rows.
3. The include checker refuses an include from `components/lib/` into `components/ports/` and from `components/ports/`
   into anything of kb-core but
   the public `kb/<part>/` headers (LAY-010, LAY-011). kb-core's own CMake never adds an MCU port as a subdirectory.
4. The **host port** is not an MCU port: it is platform-free test doubles with fault injection that every test
   needs, so it stays in kb-core as `components/lib/interfaces/host/` (target `kb::interfaces_host`), and its
   std::thread RTOS backend is `components/ports/host/` (target `kb_rtos_backend_host`), the one folder of the
   ports tree that kb-core's own CMake adds (amended by the repository layout, [layout](../layout.md)). **RTOS
   backends** stay
   in kb-core (`components/lib/rtos/backends/<rtos>/`): the RTOS API does not depend on the MCU.
5. **Extraction**: when the contracts are stable (M4 exit: interfaces at v0.4 and the first bindings proven on the
   bench), `components/ports/esp32/` moves with its history
   (`git filter-repo --subdirectory-filter components/ports/esp32`) to
   `port-esp32` ([ADR-0008](ADR-0008-repository-naming.md)), which then versions on its own; kb-core drops the
   folder and `catalog/compat.yml` gains a kb-core x port compatibility row. New families (STM32) start directly
   in their own repository. The decision to extract, or to wait, is recorded when M4 ends.

## Consequences

- kb-core's assurance scope ends at the SDK seam; each port carries its own SDK, toolchain, CI, deviations and
  SOUP list.
- Until extraction, one pull request can change a contract and its ESP32 port together.
- After extraction a contract change is a kb-core release plus a port pull request, and the board HAL pins one
  kb-core and one port.
- `KB_MCU` in kb-core still selects the MCU gate and the default RTOS, but no MCU source is compiled by kb-core.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Ports inside `components/lib/interfaces/ports/<mcu>/` (the first skeleton, handbook ADR-0013) | SDK containers, SDK-driven releases and every family's deviations inside kb-core |
| One `kb-ports` repository for all families | The same coupling between families, moved one repository over |
| A repository per family from day one | Two pull requests for every contract change while the contracts are still moving |
