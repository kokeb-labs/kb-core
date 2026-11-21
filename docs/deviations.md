# Deviations

kb-core follows MISRA C++:2023 and the KB-CPP20 profile at the handbook's `high` level
([ADR-0011](adr/ADR-0011-assurance-level-high.md)). The handbook's project-wide records (MD-CPP-001 onwards)
apply as everywhere. kb-core's own records are in `deviations/deviations.yml`, and **only** the boundary files in
`deviations/boundary-files.yml` may need them. Each MCU port keeps its own two files in
`components/ports/<mcu>/deviations/`.

| Boundary file (planned) | Records | Why |
| --- | --- | --- |
| `components/lib/rtos/backends/freertos/freertos_boundary.cpp` | kb-core (`DEV-KBC-NNN`) | FreeRTOS C API: macros used as calls, `void*` task arguments, opaque handles |
| `components/lib/rtos/backends/freertos/freertos_isr.cpp` | kb-core (`DEV-KBC-NNN`) | `...FromISR` calls and the yield macro |
| `components/ports/esp32/src/esp32_bus_boundary.cpp` | ESP32 port (`DEV-KBP-ESP32-NNN`) | ESP-IDF I2C, SPI and UART C API |
| `components/ports/esp32/src/esp32_io_boundary.cpp` | ESP32 port (`DEV-KBP-ESP32-NNN`) | ESP-IDF GPIO, ADC, TWAI and RMT C API and callbacks with `void*` user data |
| `components/ports/esp32/src/esp32_time_boundary.cpp` | ESP32 port (`DEV-KBP-ESP32-NNN`) | `esp_timer` and ROM delay |

The record format is in `deviations/README.md`. The guidelines each file needs are confirmed against the first
MISRA scan of real code, not guessed in advance; a record lists exact files, the justification, the mitigation
and a review date, and lapses unless renewed. Every change to a boundary file or a record needs a second
reviewer.
