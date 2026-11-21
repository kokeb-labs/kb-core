# components/ports/esp32/

The ESP32 family port: kb-core's contracts implemented over ESP-IDF 5.5. Its own CMake project
(`kb_port_esp32`), isolated so it can move to `port-esp32` with its history (ADR-0007, ADR-0008).

| Path | Holds |
| --- | --- |
| `CMakeLists.txt` | Under ESP-IDF: the component `kb_port_esp32`, requiring the component `kb`. Off target: a project that finds the installed `kb` package, to check the seam |
| `inc/kb/port/esp32/` | Public headers, namespace `kb::port::esp32`; no SDK type in a signature |
| `src/` | The boundary `.cpp` files (`esp32_bus_boundary.cpp`, `esp32_io_boundary.cpp`, `esp32_time_boundary.cpp`), the only files that include ESP-IDF headers |
| `deviations/` | This port's `boundary-files.yml` and MISRA `deviations.yml` (ids `DEV-KBP-ESP32-NNN`) |
| `tests/` | Host link-seam tests of each boundary file against fakes of the SDK functions it calls |
| `examples/` | ESP-IDF applications, built in CI with ESP-IDF v5.5 (`ci-esp32.yml`) |

Rules: uses only the public `kb/<part>/` headers of kb-core; nothing in kb-core's `components/lib/` includes anything
from
here; each type is `static_assert`ed against its contract; chip-variant differences use the SDK's capability
macros (`SOC_RMT_SUPPORTED`) here and nowhere else.
