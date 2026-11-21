# RTOS and SDK boundary

Where kb-core and its MCU ports touch a scheduler or an MCU SDK, and the rules for those files.

| Folder | Selected by | May include |
| --- | --- | --- |
| `components/lib/rtos/backends/none/` | `KB_RTOS=NONE` | nothing beyond the C++ subset |
| `components/lib/rtos/backends/freertos/` | `KB_RTOS=FREERTOS` | FreeRTOS headers, in its boundary `.cpp` files |
| `components/lib/rtos/backends/threadx/` | `KB_RTOS=THREADX` | ThreadX headers, in `threadx_boundary.cpp` only |
| `components/lib/interfaces/host/` | always (tests, host examples) | nothing beyond the C++ subset |
| `components/ports/esp32/src/` | the ESP32 port project (its own CMake project) | ESP-IDF headers, in its boundary `.cpp` files |

Rules:

- Public headers declare kb types only; backend and SDK handles live in opaque storage sized and aligned by the
  header.
- A boundary file converts types and maps errors; it holds no logic that needs its own test beyond that.
- Delays wait **at least** the time asked: milliseconds to ticks rounded up, plus one tick, `uint32_t` inputs.
- FreeRTOS objects use the static allocation API only, ThreadX objects `tx_*_create` on kb-owned control blocks
  and stacks; no heap.
- Calls safe from an interrupt are the `...FromIsr` functions only, and each says so in its documentation.
- FreeRTOS, ThreadX and ESP-IDF are SOUP: versions and known anomalies are recorded in [the SOUP list](assurance/soup.md)
  (the port keeps its own rows when it moves out).

kb-core's boundary files are listed in `deviations/boundary-files.yml` and their MISRA deviations in
`deviations/deviations.yml`; each port has the same two files in `components/ports/<mcu>/deviations/`
([Deviations](deviations.md)).
