# examples/

Examples are first-class: each one is a real program or project that is **built in CI**, the documentation shows its
**source through snippet includes** (never a pasted copy, so a page cannot drift from code that compiles), and it is
**listed in [`examples.yml`](examples.yml)**, which this page, the documentation, the build and the configurator
catalogue are all checked against (`tools/examples/examples.py check`, run by `tools/preflight.sh`, ctest and CI).

Examples are consumers: they use kb-core the way a HAL does, through `find_package(kb ...)` or `add_subdirectory`,
and only through the public `kb/<part>/` headers.

## Index

<!-- --8<-- [start:index] -->
<!-- examples:begin -->
| Example | Kind | Parts | What it shows | Built by | Shown in |
| --- | --- | --- | --- | --- | --- |
| `examples/host/utils_checks/main.cpp` | host | utils | CRC check values, byte packing, saturating arithmetic and wrap-safe time | `KB_BUILD_EXAMPLES`, ctest `example_utils_checks` | `docs/parts/utils.md` |
| `examples/host/rtos_thread/main.cpp` | host | rtos, utils | A CRTP thread on a 10 ms grid, a queue, a mutex, event flags, a timer, a sleep (std::thread backend) | `KB_BUILD_EXAMPLES`, ctest `example_rtos_thread` | `docs/parts/rtos.md` |
| `examples/host/interfaces_fakes/main.cpp` | host | interfaces | A register-file device on the fake I2C bus: write, write-read, a missing device, an injected fault | `KB_BUILD_EXAMPLES`, ctest `example_interfaces_fakes` | `docs/parts/interfaces.md` |
| `components/ports/esp32/examples/main/kb_utils_smoke.cpp` | esp-idf | utils | The utils_checks checks on a target, logged once | `ci-esp32.yml` (`esp32` port, ESP-IDF v5.5) | - |
| `components/ports/esp32/examples/main/kb_rtos_smoke.cpp` | esp-idf | rtos, utils | The rtos_thread checks over FreeRTOS on a target, logged once | `ci-esp32.yml` (`esp32` port, ESP-IDF v5.5) | - |
<!-- examples:end -->
<!-- --8<-- [end:index] -->

The table is generated from `examples.yml` (`python3 tools/examples/examples.py write`); do not edit it by hand.

## Boards

`boards/<name>/board.yml` are example board files for `kb image check` and `kb board gen`
([Board DMA](../docs/design/board-dma.md)); they are not programs, so they are not listed in `examples.yml`. Their
checks run in the unit tests (`kb_cli_unit`) and the generated files are compiled by ctest `kb_board_gen_host` and
the CI job `board-dma`.

## Kinds

| Kind | Where | Built and run by |
| --- | --- | --- |
| `host` | `host/<id>/` (`main.cpp`, `CMakeLists.txt`, `README.md`) | `cmake -DKB_BUILD_EXAMPLES=ON`; each is the ctest test `example_<id>`; CI: `ci-host.yml` on every pull request |
| `esp-idf` | the port's `examples/` project, one app per entry of its `app_config.yml` (ADR-0007) | the port's own CI job (`ci-esp32.yml`); they stay with the port so they move out with it |

## Adding an example

1. Make the folder (host) or the app (ESP-IDF) with a `main` source and, for a host example, a `CMakeLists.txt` that
   calls `kb_add_example(<id> REQUIRES <targets> LIBS <targets>)` (see `CMakeLists.txt` in this folder).
2. Mark the code the documentation shows with `// --8<-- [start:<name>]` and `// --8<-- [end:<name>]`.
3. Include it in the page with a snippet include of that file and section, never a copy of the code.
4. Add the record to `examples.yml`, run `python3 tools/examples/examples.py write`, then `check`.
5. `python3 configurator/tools/build_catalog.py` refreshes the catalogue's `examples` list (the check names it).

An example prints one line and exits non-zero on failure; it needs no hardware, network or file.

## Still to add

The documentation lists the examples that should exist and do not yet ([Examples](../docs/examples.md)).
