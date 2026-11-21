# examples/host/interfaces_fakes/

The bus contract `kb::iface::I2cBus` against the host port's fake bus: any function written to the concept runs
unchanged on a target port.

Build and run:
`cmake -S . -B build -DKB_BUILD_EXAMPLES=ON && cmake --build build && ctest --test-dir build -R example_interfaces_fakes`.
It prints one line and exits non-zero when a check fails. The documentation shows its source through snippet
includes ([the interfaces page](../../../docs/parts/interfaces.md)); edit the code here, never a copy there.
