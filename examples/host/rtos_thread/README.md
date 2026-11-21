# examples/host/rtos_thread/

The `kb::rtos` wrapper over the std::thread backend: a thread is a class with `Setup`, `Step` and `Cleanup`; objects are
created before use and every wait has a timeout. The ESP32 port's `kb_rtos_smoke` runs the same program over FreeRTOS.

Build and run:
`cmake -S . -B build -DKB_BUILD_EXAMPLES=ON && cmake --build build && ctest --test-dir build -R example_rtos_thread`.
It prints one line and exits non-zero when a check fails. The documentation shows its source through snippet
includes ([the rtos page](../../../docs/parts/rtos.md)); edit the code here, never a copy there.
