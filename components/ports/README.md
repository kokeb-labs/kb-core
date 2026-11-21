# components/ports/

MCU family ports, one folder each, each **its own CMake project** that uses kb-core only through its installed
package or ESP-IDF component (ADR-0007). kb-core's own build never adds an MCU port. Each port moves to its own
repository, `kokeb-labs-drivers/port-<family>` (brandless), when the contracts are stable (ADR-0008).

| Folder | Project | Future repository | Status |
| --- | --- | --- | --- |
| [`esp32/`](esp32/README.md) | `kb_port_esp32` | `port-esp32` | planned: ESP-IDF 5.5 I2C master, SPI master, UART, GPIO, ADC oneshot, TWAI, RMT, timer |
| `stm32/` | - | `port-stm32` | starts in its own repository |
| [`host/`](host/README.md) | `kb_rtos_backend_host` | stays in kb-core | the std::thread RTOS backend for PC tests, built in kb-core's own project |

`host/` is the one port that kb-core's own build adds (it has no SDK and never leaves the PC); the MCU ports are
never added from here. Its other half, the platform-free bus and pin test doubles, is `components/lib/interfaces/host/`.
