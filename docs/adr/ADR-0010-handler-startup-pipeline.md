# ADR-0010: Handler start-up is a pipeline of declared stages

Status: Accepted (owner decision, 2026-11-13).

## Context

Handbook ADR-0021 makes `EnsureInitialized()` the one start-up entry point of drivers and handlers: idempotent,
retried from scratch after a failure, and never hidden inside a read. Twenty-one drivers follow it directly. Two
chips start differently, for reasons in the chips themselves:

- **bno08x** (sensor hub, SHTP): `Begin(config)` validates the configuration, brings the port up and starts the
  reset (an NRST pulse when the port has the `SetReset` hook) and returns at once. `Poll(now_us)` then runs the
  reset wait, the hub's advertisement and the initialise handshake; `Ready()` turns true when the session is up,
  and each wait is bounded by `Config::startup_timeout_ms` (the driver enters `Faulted` when one expires). Every
  command afterwards completes asynchronously as an `OpDone` event. `Begin()` called again starts a new session.
- **se05x** (secure element, T=1 over I2C): `Open()` validates the configuration, opens the link, reads the card
  identification data, selects the IoT applet, reads its feature bits, runs the capability probes, reads the
  lifecycle state and opens platform SCP03 when the policy or the chip requires it, each wait bounded by the link.
  The platform SCP03 key handles are given before, without I/O (`SetPlatformKeys()`). `Open()` on an open device
  returns `WrongState`; `Close()` ends it; `Recover()` climbs the driver's own recovery ladder; `PowerCycle()` is
  available with a reset port.

A handler that special-cases these two in code would grow a third and fourth shape with the next chip. The
pipeline should be one runner over a declared plan, with a waiver only where a chip truly cannot fit.

## Decision

1. Every handler's `EnsureInitialized()` runs its chip's **start-up plan**, a `constexpr` list of stages declared in
   the binding (`kStartupPlan`). The runner is written once, in `HandlerCore`.
2. Stage kinds, in this order (a plan lists only the ones its chip needs):

   | Kind | Does | Typical driver call |
   | --- | --- | --- |
   | `Power` | Enable a rail or an enable pin, wait its settle time | HAL pin through the binding |
   | `HostReset` | Host-driven reset: pulse, hold, release | the driver's reset hook, or `Begin()` |
   | `Boot` | Wait the boot time or a ready condition (ready pin, advertisement) | `Poll()` until ready |
   | `Link` | Bring up a transport session (T=1, SHTP) | inside `Open()` or `Poll()` |
   | `Identify` | Read the ID, select an applet, probe capabilities | `EnsureInitialized()`, `Open()` |
   | `Secure` | Open a secure channel needed for normal use (platform SCP03) | inside `Open()` |
   | `Configure` | Apply the configuration | `EnsureInitialized()`, `SetFeature()` ops |
   | `Verify` | Read back, check the lifecycle state | a read |

3. Each stage declares: its kind (one or more), the driver call, **sync** (one call) or **polled** (repeated until
   done), its deadline (a configuration field, never a constant inside kb-core), and whether it is optional (skipped
   when the HAL supplies no pin, rail or key for it). One driver call may cover several kinds; the plan says which.
4. The runner records a `StartupReport` (the stage reached, `kb::Status`, the driver's error in `detail`, the time
   each stage took). On a failure it runs the plan's abort call (if any), destroys the driver and stays
   `Constructed`, so the next `EnsureInitialized()` starts again from the first stage (handbook ADR-0021).
5. Both thread models use the same plan ([ADR-0009](ADR-0009-handler-thread-models.md)). `StartStep(now)` runs at
   most one step of the current stage and returns `Pending`, `Ok` or the failure, so an owner task never blocks on
   a start-up. `EnsureInitialized()` is the loop over `StartStep()`, waiting with `kb::rtos::SleepFor` between
   polls, bounded by the sum of the stage deadlines; under `Guarded<H>` it holds the guarded locks for that bound.
6. The plans:

   | Chip | Plan |
   | --- | --- |
   | 21 drivers | `Identify + Configure` = `driver.EnsureInitialized()` (sync); optional `Power` and `HostReset` when the HAL supplies the pin and the driver does not drive it itself |
   | bno08x | `HostReset` (or `Link` without NRST) = `Begin(cfg)` (sync); `Boot + Link` = `Poll(now)` until `Ready()` (polled, deadline from `startup_timeout_ms`, fails on `Faulted`); `Configure` = one `SetFeature` per configured sensor, polled until each `OpDone` (deadline from `op_timeout_ms`) |
   | se05x | `Secure` keys = `SetPlatformKeys()` (sync, no I/O, optional); `Link + Identify + Secure` = `Open()` (sync, bounded by the link), skipped when `IsOpen()`; `Verify` = lifecycle state against the HAL's expectation (optional) |

7. Recovery is per plan too: the generic plan recovers with `Deinitialize()` then the plan; se05x calls the
   driver's `Recover()` (its ladder) before `Close()` and a full plan; bno08x reports a hub restart as a `Reset`
   event (health `Degraded`, the driver restores its sensors) and re-runs the plan only after `Faulted`.

### What does not fit, and the waivers

Both chips fit the handler pipeline; no handler-level waiver is needed. Two things stay outside it:

- **Applet sessions of se05x** (UserID, AESKey, ECKey) are not a start-up stage: they need credentials per use and
  the chip allows one at a time, so the handler opens them per transaction, inside a held bus transaction.
- **The driver APIs themselves** differ from handbook ADR-0021, and changing them would break what the chips need:

  | Waiver | Driver | Differs from handbook ADR-0021 | Why it cannot change | Absorbed by |
  | --- | --- | --- | --- | --- |
  | W-01 | driver-bno08x | Start is `Begin(config)` plus `Poll(now)`, not `EnsureInitialized()`; `Begin()` again restarts a running session | The hub starts asynchronously and must be polled; a blocking start would hold the bus for the whole reset and handshake | The handler's plan; `EnsureInitialized()` is idempotent at the handler |
  | W-02 | driver-se05x | Start is `Open()`, stop `Close()`; `Open()` on an open device returns `WrongState` | Open and close are session operations of the secure element (link, applet, secure channel), not a chip configuration | The handler's plan skips `Open()` when `IsOpen()` |

  The waivers are recorded here for kb-core; each driver repository records its own in its next release.

### Amendment 1 (2026-11-14): one stage per kind

A plan lists each kind once, in kind order, and `HandlerCore` checks that at compile time. Two plan rows above name
a kind twice, so they are written as follows; the calls and their order are unchanged:

- se05x: `SetPlatformKeys()` (no I/O, only when the HAL supplies key handles) is the first call of the
  `Link + Identify + Secure` stage, before `Open()`.
- bno08x without NRST: `Link` = `Begin(cfg)` (sync), `Identify` = `Poll(now)` until `Ready()` (polled), then
  `Configure`. With NRST the plan is as in the table.

Figures: [start-up pipeline](../design/handlers.md#start-up-pipeline) and
[lifecycle](../design/handlers.md#lifecycle-and-health).

## Consequences

- One start-up runner, tested once; a new chip with a different start adds a plan, not a handler shape.
- Every start-up failure names its stage, which is what bench debugging needs first.
- Owner tasks never block on a start-up; guarded callers get a bounded, blocking `EnsureInitialized()`.
- bno08x requires an owner bus ([ADR-0009](ADR-0009-handler-thread-models.md), `kRequiresOwner`).

## Alternatives considered

| Option | Why not |
| --- | --- |
| Hand-written start-up per special chip | A new handler shape per special chip; nothing shared to test |
| Change bno08x and se05x to a blocking `EnsureInitialized()` | Breaks released drivers and blocks a bus for a whole handshake |
| Waivers only, no pipeline | Leaves the two chips outside the uniform handler API |
