# examples/host/utils_checks/

The `kb::utils` helpers a driver or a HAL uses every day, checked against known values. The ESP32 port's
`kb_utils_smoke` runs the same checks on a target.

Build and run:
`cmake -S . -B build -DKB_BUILD_EXAMPLES=ON && cmake --build build && ctest --test-dir build -R example_utils_checks`.
It prints one line and exits non-zero when a check fails. The documentation shows its source through snippet
includes ([the utils page](../../../docs/parts/utils.md)); edit the code here, never a copy there.
