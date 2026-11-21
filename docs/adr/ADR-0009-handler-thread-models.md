# ADR-0009: Handler thread models, declared per bus

Status: Accepted (owner decision, 2026-11-13).

## Context

Drivers are not thread-safe and take no lock; the layer that owns a driver owns its lock and the bus lock
(every driver contract says so). Two ways of sharing a bus work in practice. One owner task per bus that serves
every device on it and publishes snapshots removes most locks and priority inversion, and suits high-rate or
timing-critical devices. A lock taken around each call suits slow, sporadic devices (an EEPROM, an I/O expander,
a power IC) used from several tasks. Mixing the two on one bus is where trouble starts: a lock-free owner and a
locking caller on the same wires. Some chips also need continuous servicing (bno08x streams reports and completes
commands asynchronously), so they cannot be served by occasional locked calls. Callers on other tasks should not
have to change their code when a bus moves from one model to the other.

## Decision

1. The handler core (`kb::handlers::HandlerCore<Derived, Driver>`) is **single-threaded by construction**: it holds
   no lock and must only be called from one context at a time. Thread safety is added around it by exactly one of
   two access facades, chosen by the thread model of its bus.
2. Each bus declares its thread model in the board configuration, and the type says it:
   `kb::handlers::BusDomain<Bus, ThreadModel::Owner>` or `BusDomain<Bus, ThreadModel::Guarded>`. A handler's
   facade must match its bus's model; a mismatch is a compile error, and the image check refuses a bus declared
   with both.
3. **Model O, owner task** (the **default for real-time buses**): one `kb::rtos::Thread` owns the bus and calls
   `Service(now)` on each of its handlers every step. `Service` runs any pending start-up stage, drains the
   handler's command queue (a bounded number per step), runs the periodic work and publishes a
   `SeqlockSnapshot`. Other tasks use `OwnerAccess<H>`: `Latest()` returns a lock-free copy of the newest
   snapshot with its sequence number and age; `Request(command, timeout)` posts to the bounded queue and waits for
   the owner's completion, returning `Busy` when the queue is full and `Timeout` when the owner does not finish in
   time. A debug-build check asserts that `Service` runs on the owner task. `LockPolicy` is `None`: the bus has no
   mutex.
4. **Model G, guarded facade** (slow, sporadic devices used by several tasks): `Guarded<H>` owns the handler and is
   the only way to reach it. Every operation `Call(timeout, operation, args...)` takes the handler's mutex, then
   the `BusDomain` mutex, both with bounded `TryLockFor`, runs the single-threaded call and releases both. The lock
   order is fixed (handler, then bus), a lock is never held across another handler or a user callback, and a call
   that cannot get both locks in time returns `Timeout` and sends nothing. This lets several tasks use a handler
   safely without any of them owning the bus.
5. **One caller API.** `OwnerAccess<H>` and `Guarded<H>` expose the same typed operations to callers (each a
   `kb::Result` with a timeout), so HAL managers and application code do not change when a bus changes model.
   `Health()` is lock-free (a seqlock snapshot) in both models.
6. A chip that must be serviced continuously declares it in its binding (`kRequiresOwner = true`, bno08x); its
   handler cannot be wrapped in `Guarded<H>` (compile error).
7. Interrupts never call handlers: an ISR sets an event flag or notifies the owner task.

Figure: [handler thread models](../design/handlers.md#thread-models).

## Consequences

- A bus has one locking story, visible in its type; mixing models on one bus cannot compile.
- Real-time buses carry no mutex and no priority inversion; slow devices stay simple to call from anywhere.
- `Request` on an owner bus costs one queue round trip; callers that only need data read `Latest()`.
- The handler core is tested once, single-threaded; the two facades are tested separately (TSan on the FreeRTOS
  POSIX port, lock-order and timeout tests).

## Alternatives considered

| Option | Why not |
| --- | --- |
| Shared mutex only | Priority inversion and lock traffic on real-time buses; cannot serve chips that need continuous polling |
| Owner task only | Forces a task per bus for a single EEPROM read now and then |
| Handler with a built-in recursive lock | Hides the locking model; recursive locks mask lock-order defects |
| A raw bus-mutex pointer passed to each handler | No owner for recovery and statistics; replaced by `BusDomain` |
