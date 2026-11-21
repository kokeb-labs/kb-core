# ADR-0008: Repository names of the kb family

Status: Accepted (owner decision, 2026-11-13); amended 2026-11-14 (port repositories are brandless and live in
`kokeb-labs-drivers`, see [Amendment](#amendment-2026-11-14)). Proposed names only: this record creates no
repository.

## Context

kb-core will be surrounded by MCU port repositories ([ADR-0007](ADR-0007-mcu-ports-isolated.md)), reference
board HALs, a hosted configurator ([ADR-0013](ADR-0013-hosted-configurator.md)) and possibly a home for tested
version sets. The drivers already have a scheme (`kokeb-labs-drivers/driver-<chip>`, brand-free, handbook
ADR-0012). Names chosen one at a time drift; a name also fixes the namespace, CMake target, package and include
root that users type.

## Decision

### Names

| Repository | Organisation | Holds | Examples |
| --- | --- | --- | --- |
| `kb-core` | `kokeb-labs` | `kb::utils`, `kb::rtos`, `kb::interfaces` (contracts and host port), `kb::handlers`, driver pins, catalogue | `kb-core` |
| `port-<family>` | `kokeb-labs-drivers` | One MCU family port: the contracts implemented over that SDK, its boundary files, examples, SOUP list | `port-esp32`, `port-stm32` |
| `kb-hal-common` | `kokeb-labs` | Shared HAL bases (handbook ADR-0005) | `kb-hal-common` |
| `kb-board-<board>` | `kokeb-labs` | A reference board HAL for a public development or evaluation board | `kb-board-esp32s3-devkitc` |
| `kb-configurator` | `kokeb-labs` | The hosted configurator site and its resolver | `kb-configurator` |
| `kb-app-template` | `kokeb-labs` | The starter project the configurator fills | `kb-app-template` |
| `kb-trains` | `kokeb-labs` | Tested version sets (`kb YYYY.N`) and their evidence, only if they outgrow the configurator repository | `kb-trains` |
| `driver-<chip>` | `kokeb-labs-drivers` | Brand-free chip drivers (unchanged) | `driver-bno08x` |
| `board-<name>` | a product organisation | A product's own board HAL; not part of the kb family, so no `kb-` prefix | `board-<name>` |

### Rules

1. Lower case, words separated by `-`, ASCII only. No product, customer or person name in any repository name.
2. The `kb-` prefix is reserved for repositories the kb family releases from `kokeb-labs`; nothing else uses it.
   Repositories in `kokeb-labs-drivers` (drivers and MCU ports) are brandless and carry no `kb-` prefix.
3. `<family>` is the lower-case `KB_MCU` value and names an SDK family, not a chip: `port-esp32` serves every
   ESP32 target (`esp32`, `esp32s3`, `esp32c6`, ...) as targets inside one repository.
4. `<board>` is the vendor's board name, lower case, without the vendor's punctuation.
5. Each repository's name fixes its code names:

   | Repository | Namespace | CMake target | Package | Include root | ESP-IDF component |
   | --- | --- | --- | --- | --- | --- |
   | `kb-core` | `kb`, `kb::utils`, `kb::rtos`, `kb::iface`, `kb::handlers` | `kb::utils`, `kb::rtos`, `kb::interfaces`, `kb::handlers` | `kb` (components) | `kb/<part>/` | `kb` |
   | `port-<family>` | `kb::port::<family>` | `kb::port_<family>` | `kb_port_<family>` | `kb/port/<family>/` | `kb_port_<family>` |
   | `kb-board-<board>` | `kb::board::<board_with_underscores>` | `kb::board_<board>` | `kb_board_<board>` | `kb/board/<board>/` | `kb_board_<board>` |

6. Every repository tags `vX.Y.Z` and `vX.Y.Z-rc.N` only, publishes its site at
   `https://kokeb-labs.github.io/<repository>/`, is MIT with copyright "The `<repository>` authors", and uses
   <kokeblabs@gmail.com> as its only public address.

Figure: [repository family](../design/ports.md#repository-family).

## Amendment (2026-11-14)

Owner decision: less branding on the repositories that sit beside the drivers.

1. MCU port repositories are **brandless** and live in the **`kokeb-labs-drivers`** organisation, next to the
   drivers they serve: `kokeb-labs-drivers/port-<family>` (`port-esp32`, `port-stm32`). The earlier name
   `kb-port-<family>` in `kokeb-labs` is withdrawn.
2. Their **code names do not change**: namespace `kb::port::<family>`, CMake target `kb::port_<family>`, package
   `kb_port_<family>`, include root `kb/port/<family>/`, ESP-IDF component `kb_port_<family>`. The code says which
   contracts it implements; the repository name says what it is. The in-repository port (`components/ports/esp32/`)
   already
   uses these code names, so extraction still renames nothing.
3. **kb-core keeps its name** and stays in `kokeb-labs`, with the other `kb-` repositories of the table.
4. Pins of an extracted port follow the driver rules ([ADR-0012](ADR-0012-pins-and-release-lock.md)): a
   `vX.Y.Z` or `vX.Y.Z-rc.N` tag, recorded by repository, tag and commit in the lock
   ([ADR-0014](ADR-0014-kb-core-is-the-assembler.md)).

## Consequences

- A user can tell from a name what a repository holds and which code names it brings.
- The in-repository port already uses its future code names (`kb::port::esp32`, `kb::port_esp32`), so extraction
  renames nothing; only the repository is named `port-esp32`.
- Creating any of these repositories is a separate owner decision; until then only kb-core exists.

## Alternatives considered

| Option | Why not |
| --- | --- |
| `kb-esp32`, `kb-stm32` | Does not say it is a port; collides with a later board or example repository |
| One repository per chip variant (`port-esp32s3`) | Duplicates the SDK seam per variant; the SDK already abstracts the variants |
| `kb-hal-<board>` for reference boards | "HAL" is already the name of the layer every board has; `board` says what it is |
| Configurator inside kb-core permanently | Its release cadence and web toolchain differ; it starts there only as a skeleton |
