# components/lib/rtos/backends/

One folder per RTOS. `KB_RTOS` picks one; the others are not compiled for a target (with `KB_BUILD_TESTS` every
backend is also built over its fake kernel). A backend folder holds the boundary `.cpp` files that call the RTOS C
API, and nothing else. Each boundary file is listed in `deviations/boundary-files.yml`; any MISRA deviation it
needs is a record in `deviations/deviations.yml`.

| Folder | `KB_RTOS` | Status |
| --- | --- | --- |
| `none/` | `NONE` | single-threaded backend with a fake clock (host default, bare-metal bring-up) |
| `freertos/` | `FREERTOS` | static allocation API, ESP-IDF or `KB_FREERTOS_INCLUDE_DIRS` |
| `threadx/` | `THREADX` | Eclipse ThreadX (ADR-0015): a product's kernel or the pinned `third_party/threadx` |
