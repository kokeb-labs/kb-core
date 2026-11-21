# ADR-0006: Interface contracts are C++20 concepts only

Status: Accepted (owner decision, 2026-11-13). Supersedes [ADR-0003](ADR-0003-concepts-vs-crtp.md).

## Context

kb-core needs contracts for buses, pins, clocks and logs that cost nothing at run time, fail at compile time with
a readable message when a port is incomplete, and match the drivers, whose ports are a CRTP base plus a concept
that checks every hook's exact signature (handbook ADR-0003).

Two facts pulled the other way. Handbook ADR-0016 allows virtual functions at three run-time boundaries and names
two of them inside kb-core: `Base*` peripheral interfaces in `interfaces/base/` and a virtual `BaseThread` with a
threads manager. And a real board can mix two implementations of one contract, such as a `GpioOutput` that is an
MCU pin for one chip and an I/O-expander pin for another. A design revision therefore proposed a concept plus a
small abstract `Base*` class for every contract. The owner decided against it: one contract per bus, written
once, in modern C++20.

## Decision

1. **C++20, and only C++20.** kb-core is written for C++20 (`CMAKE_CXX_STANDARD 20`, required, no extensions).
   There is no fallback for an older standard and no test of `__cplusplus` to select one.
2. **Every contract is a concept** in `kb::iface` (`I2cBus`, `SpiDevice`, `UartPort`, `GpioOutput`, `GpioInput`,
   `AdcChannel`, `CanBus`, `PulseTx`, `MonotonicClock`, `BusyWait`, `LogSink`, ...). No base class carries a
   contract. Every port proves conformance with a `static_assert(kb::iface::I2cBus<Port>)` beside its definition,
   and compile-fail tests prove that incomplete types are rejected.
3. **No virtual functions in kb-core.** kb-core uses none of the virtual boundaries handbook ADR-0016 permits:
   there are no `Base*` interface classes, and `kb::rtos` threads are a CRTP base, not a virtual `BaseThread`. A
   registry of threads holds non-virtual control blocks (stack statistics, liveness, a stop flag), never pointers
   to a polymorphic base.
4. **A base class exists only where shared behaviour truly needs it**, and each one is a row of the register
   below with its reason. A base that would only forward calls is never written. Adding a row needs a new ADR or
   an amendment of this one.

   | Base class | Kind | Why it needs a base |
   | --- | --- | --- |
   | `kb::handlers::I2cHooks<Derived, Bus>`, `SpiHooks`, `UartHooks`, `AdcHooks`, `GpioHooks` | CRTP | They implement a driver's hooks once for every chip on that bus: failure recording, counters, the at-least delay split (busy-wait below 1 ms, `kb::rtos::SleepFor` from 1 ms) and clock narrowing with wrap-safe arithmetic. Without them every binding repeats four pieces of correctness-critical code |
   | `kb::rtos::Thread<Derived, StackBytes>` | CRTP | The run loop, fixed-rate waiting with overrun counting, stop handling, step-time statistics and the liveness beat are written once; a derived thread supplies only `Setup`, `Step` and `Cleanup` |
   | `kb::handlers::HandlerCore<Derived, Driver>` | CRTP | The lifecycle state machine, the health record and the start-up pipeline runner ([ADR-0010](ADR-0010-handler-startup-pipeline.md)) are written once for every handler |

5. **Run-time choice without virtual functions.** Where a HAL must choose an implementation at run time, it uses a
   type-erased reference from `kb::iface` (`GpioOutputRef`, `I2cBusRef`, `LogSinkRef`, ...): a non-owning value
   holding a context pointer and a pointer to a constant table of function pointers, produced from any type that
   satisfies the concept by one template, and itself checked with `static_assert` against the same concept. A
   `*Ref` is added for a contract only when a HAL needs one; each is listed in the contract's documentation as a
   type-erasure point.

### Relation to handbook ADR-0016

Handbook ADR-0016 permits virtual functions at its three boundaries; it does not require them. kb-core is
stricter, which the handbook check accepts: `.kokeb/handbook.yml` lists no virtual-boundary folder, so the
`virtual-outside-interfaces` check rejects any `virtual` anywhere in kb-core. Two statements of the handbook no
longer describe kb-core and are proposed for amendment: boundary 1 (`Base*` interfaces in kb-core's
`interfaces/base/`) and the kb-core half of boundary 3 (`BaseThread`, `BaseThreadsManager`). Boundary 2
(kb-hal-common's managers and sinks) is outside kb-core and unchanged. The layout of handbook ADR-0013
(`interfaces/base/`, `interfaces/mcu/<family>/`) is amended by [ADR-0007](ADR-0007-mcu-ports-isolated.md).

## Consequences

- One contract per bus, written once; nothing can drift between a concept and a parallel class.
- Static dispatch everywhere; an incomplete port fails at compile time with the concept's name.
- A run-time choice costs one indirect call per operation and is visible in the code as a `*Ref`.
- The MISRA C++:2023 rules on virtual functions and hierarchies never apply to kb-core code.
- Generic code over contracts is templated; build times are watched and headers stay small.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Concept plus a small abstract `Base*` per contract | Two forms of each contract to keep in step; a vtable per port; rejected by the owner |
| Abstract base classes only | Virtual dispatch on every transfer and no compile-time check of the hook signatures |
| CRTP base for every contract | Forwarding boilerplate with no behaviour, and harder diagnostics than concepts |
| `std::function` or a virtual adapter for run-time choice | Allocation or a vtable; the `*Ref` gives the same choice without either |
