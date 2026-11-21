# components/lib/rtos/backends/threadx/

The Eclipse ThreadX backend of `kb::rtos` (ADR-0015), selected with `-DKB_RTOS=THREADX`.

| File | What it is |
| --- | --- |
| `threadx_boundary.cpp` | The only kb-core file that includes ThreadX headers (`deviations/boundary-files.yml`): every `kb::rtos::backend` function over `tx_*` calls, static allocation only |
| `threadx_isr.cpp` | The `...FromIsr` functions: the thread-context calls with no wait, which ThreadX allows from an interrupt |
| `CMakeLists.txt` | Where the kernel comes from: `KB_THREADX_INCLUDE_DIRS` (a product's kernel, with `KB_THREADX_DEFINITIONS` and the required `KB_THREADX_TICK_HZ`) or `KB_THREADX_PORT` (the pinned `third_party/threadx`); neither stops the configure |

The configuration a kernel must have, the differences from FreeRTOS and the tests are on the site page
[ThreadX backend](../../../../docs/parts/rtos-threadx.md).

The `tx_user.h` kb-core builds the pinned kernel with (tests, cross-compiles) is `components/lib/rtos/threadx-config/kb_tx_user.h`,
outside this directory because it is build configuration, not target code; a product uses its own.
