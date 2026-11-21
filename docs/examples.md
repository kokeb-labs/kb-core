# Examples

Examples are first-class in kb-core. Each one is a real program or project that CI builds; the documentation shows
its source through snippet includes, never a pasted copy, so a page cannot drift from code that compiles; and each
is listed in one index, `examples/examples.yml`, which the build, the README table, the documentation and the
configurator catalogue are checked against (`tools/examples/examples.py check`).

## Index

--8<-- "examples/README.md:index"

## Rules

- A **host** example is a folder under `examples/host/` with `main.cpp`, `CMakeLists.txt` and `README.md`. It is
  built with `-DKB_BUILD_EXAMPLES=ON` (the host job of CI does so on every pull request) and runs as the ctest test
  `example_<id>`. It uses the strict flags of the library, needs no hardware and prints one line.
- An **ESP-IDF** example is an app of the ESP32 port's example project (ADR-0007), listed in that port's
  `app_config.yml` and built by its own CI job with ESP-IDF v5.5. It stays with the port so that it moves out with it.
- The code a page shows sits between `// --8<-- [start:<name>]` and `// --8<-- [end:<name>]` in the example's
  `main.cpp`, and the page includes it with a snippet include of that file and section.

## Still to add

The parts have one example each. These are missing; each is a small, hardware-free program in the same form.

| Example | Part | What it should show |
| --- | --- | --- |
| `handlers_bus_owner` | handlers | One driver binding on the owner thread model over a host bus and the driver's chip model: start-up pipeline, a command, a health read |
| `handlers_guarded_bus` | handlers | Two bindings sharing one bus with the guarded thread model, and a fault injected on the bus |
| `handlers_recovery` | handlers | A handler faulting, backing off and recovering, with the health state printed at each step |
| `interfaces_uart_loopback` | interfaces | The fake UART with a bounded read, and a framing helper written to the concept |
| `interfaces_spi_pins` | interfaces | The fake SPI bus and the pin contract: chip select handling and a failed transfer |
| `rtos_isr_queue` | rtos | An interrupt-context send into a queue and a thread draining it, on the FreeRTOS fake kernel |
| `utils_containers` | utils | `FixedString`, the fixed containers and checked arithmetic with no heap |
| `install_consumer` | all | A project outside this repository that installs kb-core and uses it through `find_package(kb)` |
| `kb_image_project` | all | A generated project (the configurator's output) building a driver binding for a target |
| `stm32_blink_rtos` | rtos, interfaces | An STM32 port example, once the port repository has its first release |
