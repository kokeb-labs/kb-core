# kb::handlers

Platform-free glue between the chip drivers in `components/drivers/` and the contracts of `kb::interfaces`, used only by
the
HAL (docs: `docs/handlers/`, design: `docs/design/handlers.md`).

- `inc/kb/handlers/common/`: the single-threaded handler core (`HandlerCore`: lifecycle, start-up pipeline runner
  over a declared `kStartupPlan`, health record, snapshot, recovery), `BusDomain<Bus, ThreadModel>` and the two
  access facades `OwnerAccess<H>` (model O) and `Guarded<H>` (model G) with one caller API, the request queue, the
  CRTP hook bases per bus (ADR-0006 register), the seqlock and the liveness monitor.
- `inc/kb/handlers/bindings/<name>.hpp`: one binding and handler per driver: the port over kb contracts, `ToStatus()`
  for every driver error (no `default`), the plan, `kRequiresOwner`, `kBus`, and the app calls.
- `tests/`: unit tests of the core (none backend), thread tests (host backend) and compile-fail tests; `fuzz/`: the
  request queue. Driver tests are in `tests/integration/`.

Rules: no MCU, RTOS or board knowledge (board values arrive as configuration); no virtual functions; a driver is
never modified to suit a handler; every mutating call of a core needs a `ServiceKey`, which only a facade makes.
