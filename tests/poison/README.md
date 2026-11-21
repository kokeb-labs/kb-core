# tests/poison/

Recompiles every platform-free translation unit of `components/lib/` (all of it except `rtos/backends/*`; MCU ports are
outside `components/lib/`) with `ESP_PLATFORM`, `USE_HAL_DRIVER`, `configTICK_RATE_HZ=100` and other SDK macros
defined and no SDK include path, then runs the host tests on the result. A difference means platform code
leaked above the boundary.
