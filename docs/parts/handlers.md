# kb::handlers

Platform-free glue between drivers and kb contracts. Used only by the HAL. Each driver has a **binding** (its port
over kb contracts, the one map of its errors to `kb::Status`, its start-up plan and thread-model needs) and a
**handler** on the shared single-threaded core (`HandlerCore`), reached through the access facade its bus declares
(owner task or guarded).

| Piece | Names |
| --- | --- |
| Hook bases | `I2cHooks`, `SpiHooks`, `UartHooks`, `AdcHooks`, `GpioHooks` (CRTP), `HookTiming` |
| Bindings | `kb/handlers/bindings/<name>.hpp`: `Port`, `ToStatus()`, `Config`, `Snapshot`, `Command`, `Handler`, app calls |
| Core | `HandlerCore<Derived, Driver, Snapshot, Command, Reply>`, `Stage`, `StartupReport`, `HealthRecord`, `ServiceKey` |
| Bus and access | `BusDomain<Bus, ThreadModel>`, `BusHold` (`Hold()`), `OwnerAccess<H, Domain>` (`Held()`), `RequestChannel`, `Guarded<H, Domain>` (`Hold()`), `GuardedHeld`, `OwnerHeld` |
| Non-blocking reads | `AsyncPoller<Op, Bytes, Use>`, `AsyncUse`, `AsyncStats` |
| Supervision | `LivenessBeat` (`Park()`, `Resume()`), `LivenessMonitor<N>` (`Verdict()`), `Seqlock<T>`, `HealthJournal<Sink, N>` |

Build: `-DKB_ENABLE_<NAME>=ON` links a driver into `kb::handlers`; `-DKB_BUILD_TESTS=ON -DKB_BUILD_INTEGRATION=ON`
compiles and tests every binding whose driver is checked out.

Pages: [Handlers](../handlers/index.md) (overview and one page per handler); design:
[Handlers](../design/handlers.md), [Bus access](../design/bus-access.md),
[Fault evidence](../design/fault-journal.md).
