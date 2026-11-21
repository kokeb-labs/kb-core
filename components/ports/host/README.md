# components/ports/host/

The host port. **Test code, outside the MISRA claim.** The `host` RTOS backend (`KB_RTOS=HOST`): `kb::rtos` over
`std::thread`,
`std::mutex` and `std::condition_variable`, so the typed API, the CRTP thread base and multi-threaded handler code
run for real on a PC, under ThreadSanitizer as well. It is a PC-only backend: it allocates (the standard library
does), uses exceptions-free standard threading only, and is never built for a target. It lives in the ports tree, not in
`components/lib/`, because `components/lib/` may not include `<thread>` or `<mutex>` (handbook KB-CPP-012).

| Path | Holds |
| --- | --- |
| `CMakeLists.txt` | The target `kb::rtos_backend_host`; added by `components/lib/rtos` for `KB_RTOS=HOST` and for host test builds |
| `src/` | `host_backend.cpp`, the only source |

The other half of the host port, the platform-free bus and pin test doubles, is `components/lib/interfaces/host/`
(header-only, `kb::interfaces_host`). Unlike `esp32/` and `stm32/`, this port is built inside kb-core's own project:
it has no SDK and never leaves the PC.

Time: `Now()` is `std::chrono::steady_clock`; the tick is 1 ms (1000 Hz), derived from the same clock.
