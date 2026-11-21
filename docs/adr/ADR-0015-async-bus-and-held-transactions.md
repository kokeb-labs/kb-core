# ADR-0015: Non-blocking bus access and held bus transactions

Status: Proposed (2026-11-14), for the owner's decision.

## Context

Every kb contract call is bounded (ADR-0006), but bounded is not the same as cheap or safe to wait on. Three
lessons from earlier work set the requirements:

- A bus wrapper that busy-polled the peripheral for every transfer cost about 8 % of a core for one sensor read at
  its control rate. An interrupt-driven start/poll path, added beside it later, removed that cost.
- A safety task waited inside a vendor I2C call until the watchdog fired. Safety paths must never wait on a bus;
  the safe-state latch (outputs cut first, no bus touched) is a HAL rule, but the library must make the
  non-waiting path the easy one.
- Several drivers need a whole call, or a sequence of calls, held on one bus: as5047u answers a read in the next
  frame, at25040b writes and reads ranges of several chunks, ads7952 programs a mode and then reads frames, and
  se05x T=1 exchanges blocks that belong together. A guarded handler already runs each driver call under the bus
  lock (ADR-0009); a sequence of calls was not covered, and a range of chunks could be interleaved by another task.

The synchronous contracts (`I2cBus`, `SpiDevice`) stay as they are: every driver port is synchronous, and start-up
and slow paths are fine with a bounded wait.

## Decision

1. **Asynchronous contract** (`kb/interfaces/async.hpp`). `kb::iface::AsyncOperation` (`Poll(now)` returning
   `AsyncPoll`, `Cancel()`, `kNonBlocking`), `I2cAsync` (`StartWrite`, `StartWriteRead`, `StartRead`) and
   `SpiAsync` (`StartTransfer`), each also the synchronous contract. Start takes a `kb::utils::Deadline` and
   returns at once (`Ok`, `Busy`, `InvalidArgument`, `Timeout`, `NotReady`). `Poll()` never waits: `Idle`,
   `Pending`, or `Done` with the outcome as a `kb::Result<std::size_t>` (`Completion()`), reported exactly once.
   A transfer not complete by its deadline completes as `Timeout`, and so does a completion that arrives after the
   deadline (late data is never `Ok`). `Cancel()` is bounded; whether it or a racing completion wins, it returns
   `Ok`, the completion is dropped, and nothing touches the caller's buffers afterwards. Completion is signalled
   from the ISR by the port (an event flag of the owner task, or nothing: the owner polls once per pass).
2. **Ports may adapt now.** `kb::iface::AsyncFromSync<Bus, Clock>` serves the contract from any synchronous port:
   Start() runs the whole transfer, keeps the deadline rules, and `kNonBlocking` is false. A hot path does not
   compile over it: `kb::handlers::AsyncPoller<Op, Bytes>` requires `NonBlocking<Op>` unless it is declared
   `AsyncUse::MayBlock`. So ports are not forced to implement the native form before a handler needs it, and the
   type system says where a wait is still hidden.
3. **Owner-task use** (`kb::handlers::AsyncPoller`). It owns the transfer's buffers (they outlive the transfer by
   construction), starts a transfer with a budget, and `Step(now)` polls once per pass and reports the completion
   once; the destructor and `Cancel()` end a transfer in flight. A hot-path access of an owner bus starts a read in
   one pass and publishes it in a later one; no pass waits on the bus. Drivers keep their synchronous hooks: a
   binding uses the poller for reads it frames itself (one register, one frame), and a driver-level async hook is
   a later template change.
4. **Held transactions.** `BusDomain::Hold(timeout)` returns a `[[nodiscard]]`, move-only RAII `BusHold`, made only
   by the domain: on a guarded bus the bus mutex (bounded wait), on an owner bus the pass flag (never waits; a hold
   and a service pass exclude each other). `Bus()` is null after `Release()`, and `Bus()` on a temporary hold does
   not compile. For handler calls:
   - `Guarded<H>::Hold(timeout)` returns a `GuardedHeld`: the handler mutex, then the bus, in the order of every
     call (ADR-0009). It has the facade's `Request` and `Sample`, so a binding's app call runs whole under it
     (`at25040b::Write(held, ...)`). There is no `Held(BusHold&)` on a guarded handler: taking the handler lock
     while holding the bus would invert the lock order.
   - `OwnerAccess<H>::Held(BusHold&)` returns an `OwnerHeld`: the owner task, between passes, runs calls inline under
     its hold of the bus, several handlers of the bus under one hold. A hold of another bus type does not compile; a
     hold of another domain object, or a released one, refuses every call.
   - Every wait is bounded, so a hold taken across two buses in opposite orders by two tasks ends in `Timeout`,
     never a hang; the rule is still: never hold two buses.
5. **Safety rule.** A safety path never calls a bus: it cuts outputs first and reads only published snapshots
   (`Latest()`, `Health()`, the liveness verdict). The HAL documents its safety latch; kb-core gives no API that
   lets a safety task wait on a bus without a timeout.

## Consequences

- A hot-path read costs one Start() and one Poll() per pass instead of a busy wait; where a port has no native form
  yet, the wait is visible in the type (`MayBlock`) and in review.
- The held calls give multi-chunk and multi-call sequences the same atomicity a single guarded call has, in both
  thread models, without a recursive lock.
- `BusHold` is no longer constructed directly; code that built one with `{domain, timeout}` uses `domain.Hold()`.
- An owner-bus hold taken from another task is not refused by a task check (there is none): it excludes the pass
  and the pass counts a violation, so a misuse shows in the domain's statistics and never races the handler.
- Ports have work to do before a native hot path exists (below); until then `AsyncFromSync` keeps every handler
  building.

## What each port must add

| Port | Asynchronous contract | Held transactions | Fault journal store, reset reason |
| --- | --- | --- | --- |
| `components/ports/esp32` (to move to `port-esp32`) | `I2cAsync` over the `i2c_master` asynchronous mode (`trans_queue_depth` > 0, `i2c_master_register_event_callbacks`, completion sets the owner's event flag from the callback); `SpiAsync` over `spi_device_queue_trans` and `spi_device_get_trans_result` with a zero timeout in Poll(); Cancel() waits at most one queued transaction (the driver cannot abort one) and then drops it; `kNonBlocking = true` only when both hold | Nothing: holds are in kb-core | A store in RTC slow memory (`RTC_NOINIT_ATTR` placement of a `RamJournalStore`) and one over an NVS blob or a flash partition with one write unit per slot; `esp_reset_reason()` mapped to `kb::utils::ResetReason` with the raw value in `detail` |
| `port-stm32` (planned, M6) | `I2cAsync` over `HAL_I2C_Master_Transmit_IT`/`_DMA` and the completion callbacks, `HAL_I2C_Master_Abort_IT` for Cancel(); `SpiAsync` over `HAL_SPI_TransmitReceive_DMA` and `HAL_SPI_Abort`; the known defects of older STM32 bus wrappers as its first tests | Nothing | Backup SRAM or the RTC backup registers as a store, emulated EEPROM in flash as a second; the `RCC_CSR` reset flags mapped to `ResetReason`, then cleared |
| Host (`components/lib/interfaces/host`) | `FakeAsyncI2c`, `FakeAsyncSpi` (done) | Nothing | `RamJournalStore` (done) |

## Alternatives considered

| Option | Why not |
| --- | --- |
| Make every contract call asynchronous (futures or callbacks) | Every driver port is synchronous; start-up and slow paths gain nothing and lose simplicity; callbacks from an ISR into handler code break ADR-0009 rule 7 |
| Completion callbacks instead of Poll() | A callback runs in the ISR or a port task, outside the handler's one context; the owner pass would need a lock again |
| A `Status::Pending` value in `kb::Status` | The closed error enum would mean "not an error" for one value; every switch over `kb::Status` would change; `AsyncPoll` keeps state and outcome apart |
| Coroutines (C++20) | Frame allocation needs a custom allocator per coroutine, debuggers and the MISRA checker handle them poorly, and the owner pass already is the scheduler |
| Require native async in every port now | Blocks handler work on port work; `AsyncFromSync` plus the `NonBlocking` gate gives the same safety with a visible migration path |
| A recursive bus lock for sequences | Hides the lock order and masks lock-order defects (ADR-0009 already rejects it) |
| `Held(BusHold&)` on guarded handlers | Takes the handler lock while holding the bus: the reverse of every other call's order |
| A bus hold that is copyable or a plain scope guard with a public constructor | A copy releases twice; a guard built anywhere cannot carry the domain's statistics or the owner-pass exclusion |
| A task-identity check for owner holds | The RTOS API has no portable cheap task id at this layer; the pass flag gives mutual exclusion in every build and counts misuse |
