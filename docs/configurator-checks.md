# Configurator checks

The configurator runs two sets of rules on every change. The **shared rules** are the resolver specification
([Assembler and resolver](design/assembler.md), codes `KB-RES-NNN`): `configurator/resolver.mjs` implements them
exactly as the `kb` tool does, proven by the shared vectors in `tests/resolver/vectors/`, and it writes the lock,
the CMake files and `kb-fetch.sh`. The **site checks** below (codes `CFG-<area>-NNN`, `configurator/js/resolver.js`)
explain the same problems in plain words and add what only the site knows from its catalogue: bus instances of
the target, point-to-point lines, I2C addresses, the pin budget, handler bindings, tested version sets and known
vulnerabilities. Where a site check covers a shared rule, the page shows one finding with both codes.

A selection is offered for download only when both sets find no error.

## Inputs

- **Site catalogue** (`configurator/data/catalog.json`, schema `configurator/schemas-draft/catalog.schema.json`): a
  superset of kb-core's catalogue (`catalog/schemas/catalog.schema.json`) with labels, targets and their bus instance
  counts, boards and their free GPIO (where known), bus kinds, chip cards, scans and tested sets.
  `configurator/js/shared.js` derives kb-core's catalogue from it for the shared rules.
- **Selection** (schema `configurator/schemas-draft/kb-image.schema.json`): image name, kb-core version, port
  version (`null` for an in-tree port), MCU family, target and board, RTOS, the drivers (name, version, the bus value
  used `via`, the bus `bus`) and the thread model of each bus. `shared.js` turns it into `kb-image.yml`.

## Bus instances

A bus is named `<instance><index>`: `i2c0`, `spi1`, `uart1`, `can0`, `pio0`, `adc0`. Each driver bus value maps to
an instance kind and a pin cost (`bus_kinds` in the catalogue): `i2c` to `i2c` (2 shared pins), `spi` to `spi` (3
shared pins and a chip select per device), `uart` to `uart` (2 pins, one driver), `modbus` to `uart` (an RS-485
line shared by Modbus devices, 3 pins), `can` and `canopen` to `can` (2 pins), `adc` to `adc` (one channel and pin
per device), `pio` to `pio` (one RMT channel and pin per driver on ESP32). A target lists how many instances of each
kind it has. In `kb-image.yml` a bus whose name does not give the bus value (Modbus on `uart1`, CANopen on `can0`)
carries an explicit `kind`.

## Thread models

Every bus has exactly one model, `owner` or `guarded` ([ADR-0009](adr/ADR-0009-handler-thread-models.md)). The
site's default is `owner`, except `guarded` for I2C and ADC buses without a chip that needs an owner task.

## Tested sets

A selection belongs to a tested set when kb-core, the port and every driver are at that set's versions (a subset
of the set is allowed) and the set has a passing configuration for the same MCU family, target and RTOS. A
selection with a driver that did not resolve belongs to none.

## Codes

| Code | Severity | Rule | Shared rule |
| --- | --- | --- | --- |
| `CFG-MCU-001` | error | The MCU family is in the catalogue | `KB-RES-003` |
| `CFG-MCU-002` | error | The target belongs to the family | `KB-RES-005` |
| `CFG-MCU-003` | error | The board belongs to the family and carries the selected target | |
| `CFG-MCU-004` | error | The family has a port (it is not `planned`) | `KB-RES-004` |
| `CFG-RTOS-001` | error | The family offers the RTOS (ESP-IDF runs FreeRTOS only) | `KB-RES-008` |
| `CFG-RTOS-002` | error | The RTOS backend is implemented (not `planned`) | `KB-RES-007` |
| `CFG-RTOS-003` | error | The RTOS is in the catalogue | `KB-RES-006` |
| `CFG-KB-001` | error | A kb-core version is selected | `KB-RES-001` |
| `CFG-KB-002` | error | The kb-core version is in the catalogue | `KB-RES-002` |
| `CFG-KB-003` | warning | kb-core is a development revision (no tag yet) | `KB-RES-W03` |
| `CFG-PORT-002` | error | An extracted port's version exists; an in-tree port takes no version of its own | `KB-RES-020` |
| `CFG-PORT-003` | error | The port release's `kb_core` range holds the selected kb-core version | |
| `CFG-PORT-004` | info | The port is still inside kb-core and comes with it | |
| `CFG-RC-001` | warning | A pin (kb-core, port or driver) is a release candidate; one finding per pin | `KB-RES-W01` |
| `CFG-DRV-001` | error | The driver is in the catalogue | `KB-RES-009` |
| `CFG-DRV-002` | error | A driver is selected once | `KB-RES-010` |
| `CFG-DRV-003` | error | The driver has published versions and the selected version is one of them | `KB-RES-011` |
| `CFG-DRV-010` | error | The version's `mcus` is `any` or holds the family (the MCU gate) | `KB-RES-013` |
| `CFG-DRV-011` | error | The version's `kb_core_min` is not newer than the selected kb-core | `KB-RES-014` |
| `CFG-DRV-012` | info | `kb_core_min` is set but no kb-core is chosen | |
| `CFG-HND-001` | error | The selected kb-core release has a handler binding for the driver | |
| `CFG-HND-002` | info | Against a development revision of kb-core, the binding cannot be checked | |
| `CFG-BUS-001` | error | `via` is one of the version's bus values and a known bus kind | `KB-RES-019` |
| `CFG-BUS-002` | error | The bus name is `<instance><n>` for the instance kind of `via` | |
| `CFG-BUS-003` | error | The bus index exists on the target (not for `adc` and `gpio`) | |
| `CFG-BUS-004` | error | A point-to-point bus (plain `uart`, `pio`) carries one driver | |
| `CFG-BUS-005` | error | One bus carries one protocol (`uart` and `modbus` do not mix) | |
| `CFG-BUS-008` | warning | A thread model names a bus no driver uses | |
| `CFG-BUS-010` | error | The I2C devices of a bus whose addresses are known can each get their own address | |
| `CFG-BUS-011` | info | They can, but some must be set apart; an example assignment is given | |
| `CFG-BUS-012` | info | A device on a shared I2C bus has no known address | |
| `CFG-BUS-013` | error | The ADC devices fit the target's ADC channels | |
| `CFG-THR-001` | error | A bus has the model `owner` or `guarded` | `KB-RES-015` |
| `CFG-THR-002` | error | A chip that needs continuous service (`requires_owner`, `kRequiresOwner`) is not on a guarded bus | `KB-RES-018` |
| `CFG-THR-003` | info | With RTOS `none`, an owner bus is serviced from the main loop | |
| `CFG-GPIO-001` | error | The bus pins fit the board's free GPIO, or the target's GPIO when the board's is not known | |
| `CFG-GPIO-002` | warning | The bus pins fit but not with every optional control pin | |
| `CFG-GPIO-003` | info | The board's free GPIO is not known; the needed range is given | |
| `CFG-SET-001` | warning | Untested combination: no tested version set holds the selection | |
| `CFG-SET-002` | info | The selection belongs to the named tested set | |
| `CFG-CVE-001` | error or warning | A known vulnerability in a selected version: error for high or critical, warning otherwise | |
| `CFG-CVE-002` | info | A selected component has not been scanned for known vulnerabilities | |

## Vectors

One JSON file per case (schema `configurator/schemas-draft/resolver-vector.schema.json`) in
`configurator/tests/vectors/resolver/`: `catalog` (a path relative to the vector), `image`, and `expect` with `ok`,
`errors`, `warnings`, optionally `infos` and a partial `resolved` compared key by key. They run against
`configurator/tests/vectors/catalog-fixture.json`. The configurator's tests also run every shared vector of
`tests/resolver/vectors/` through `configurator/resolver.mjs`, and check on every site vector that the site never
accepts a selection the shared rules refuse.

## Exports

The downloads are tested end to end, as a user gets them. `configurator/tools/export-matrix.json` lists the
selections:

| Row | Platform | Drivers | Build |
| --- | --- | --- | --- |
| `host-only` | host, no RTOS | none | host: app, every header, run |
| `host-shared-i2c` | host, `std::thread` | five on one guarded I2C bus, three on an owner SPI bus | host |
| `esp32c6-freertos` | ESP32-C6, FreeRTOS | bmp581 and pcal9555 on one I2C bus | ESP-IDF v5.5 |
| `esp32s3-freertos` | ESP32-S3, FreeRTOS | bno08x (SPI, owner), ws2812_rmt (RMT), tmc9660 (UART) | ESP-IDF v5.5 |
| `stm32-few` | STM32H7, FreeRTOS | three | refused: the family has no port yet (`KB-RES-004`) |
| `cortexm-few` | host family, cross-compiled | the same three | Cortex-M4 with arm-none-eabi-g++ |
| `esp32s3-every-driver` | ESP32-S3, FreeRTOS | every driver of the catalogue | ESP-IDF v5.5 |
| `host-every-driver` | host, `std::thread` | every driver that runs off ESP32 | host |

1. `node configurator/tools/export_matrix.mjs OUT --chrome` makes each selection with the page's own code and, in
   headless Chrome, clicks every **Download** button of the Files panel, the ZIP button and the offline copy; each
   download must equal the Node result byte for byte. A refused row must fail with its code.
2. `python3 configurator/tools/validate_exports.py OUT` validates the SPDX 2.3 SBOM with the SPDX project's
   `spdx-tools` and the CycloneDX 1.6 SBOM against the official schema (CycloneDX's `cyclonedx-python-lib`, strict),
   `kb-image.yml` and the lock against kb's schemas, the licence report against the lock and both SBOMs, and checks
   that `kb resolve` writes the same lock and build files as the page.
3. `configurator/tools/export_build.sh OUT/<row>/project host|cortex-m|esp-idf [KB_CORE_DIR]` runs the project's
   `kb-fetch.sh` (every submodule at its locked tag and commit) and builds it as its README says. The board file
   includes the handler binding of every selected driver, so the build compiles each one. With `KB_CORE_DIR`,
   kb-core inside the project is moved to that checkout first.

CI (`.github/workflows/ci-exports.yml`) runs the rows marked `ci` on every change to the configurator, the kb
tool, the libraries or the build files, with kb-core at the commit under test: export through Chrome, validate, then
the host, Cortex-M and ESP-IDF builds.
