# Design rules and their reasons

Rules kb-core follows everywhere, each with the failure it prevents. They come from experience with embedded
stacks of this kind and from the drivers' own review history; each becomes a test, a check or a review item.

| Area | Rule | Prevents |
| --- | --- | --- |
| Ownership | One composition root per image (the board facade), built statically and started explicitly, in a fixed order, after the scheduler runs. kb-core types have trivial or `constexpr` constructors that never touch the RTOS or hardware; no kb-core type is a singleton | RTOS objects created in static constructors leaving the interrupt mask raised and wedging boot; singletons that block test injection |
| Lifecycle | `[[nodiscard]] kb::Result<void>` for every start; the ordered start fails closed and records which step failed | A start whose result is ignored, and a failed chip showing up later as a null object |
| Errors | One `kb::Result<T>` with a `detail` field; ports put the vendor code there | Several error styles side by side; losing the vendor status that bench debugging needs |
| Bus sharing | One thread model per bus, declared ([ADR-0009](../adr/ADR-0009-handler-thread-models.md)) | Priority inversion and lock traffic on real-time buses; a lock-free owner and a locking caller on one bus |
| Blocking | Every interface call takes or implies a timeout; hot paths use the asynchronous `Start`/`Poll` form ([bus access](bus-access.md)); safety paths never wait on a bus | A busy-polling bus wrapper eating a core; a safety task stuck in a vendor bus wait until the watchdog fires |
| Time | One 64-bit monotonic microsecond clock above the ports; delays wait at least the time asked; elapsed time by unsigned subtraction | Round-down delays; a vendor tick that stalls under the RTOS; clock-wrap defects after 2^31 or 2^32 |
| Threads | The CRTP thread base carries `Setup`/`Step`/`Cleanup`, fixed-rate waiting with overrun counts, a fail-closed start, park and wake, step-time and stack statistics | Instrumentation added by hand per thread, or not at all |
| Liveness | `kb::handlers::LivenessMonitor`: a beat per handler or thread with a stall budget; parked ones declare it (`Park()`); the watchdog owner reads the verdict ([fault evidence](fault-journal.md)) | A watchdog kicked by a loop that is alive while the work is not |
| Boundaries | Include checks plus a symbol lint on linked objects; no allowlist files, only deviation records with a review date | Leaks include checks miss (macros, inline SDK calls through a re-exported header); allowlists that only grow |
| Configuration | Presets emitted by the image generator; no product macro in kb-core; one source per fact (`board.yml`, the lock files) | Product `#if` spread through shared code; two sources of pin data that drift apart |
| Testing | Host-first tests per part; HAL-free logic tested with recorded-data replay; bench evidence under `docs/assurance/bench/` in a fixed form | Code proven only on a bench run nobody can repeat |
| Debt | Every workaround cites an issue or a deviation id; a lint counts untracked `workaround` and `errata` comments | Workarounds recorded as prose that nobody tracks |
| Provenance | Code written fresh under the pipeline ([ADR-0005](../adr/ADR-0005-no-code-port.md)) | Carrying unknown defects and an unclear licence into kb-core |
