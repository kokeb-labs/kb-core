# tests/integration/

Host tests that drive each pinned driver through its binding and handler in `kb::handlers`, over `kb::interfaces`
contracts backed by the driver's own chip model or mock (from the driver's `tests/` folder), in both thread models
(owner only where the chip requires it), on the std::thread RTOS backend. Each folder `<name>/` covers the start-up
plan and its report, calls before a start, start failures with the right `kb::Status` and the driver's code in the
detail, the app calls, the error map into health, recovery, and the owner and guarded facades. Build with
`-DKB_BUILD_TESTS=ON -DKB_BUILD_INTEGRATION=ON` (every checked-out driver).
`common/binding_support.hpp` holds the shared clock, timing and owner loop.
These tests do not repeat a driver's own unit tests. The compile-and-link matrix of every binding over every port
and configuration is driven from `catalog/compat.yml`.
