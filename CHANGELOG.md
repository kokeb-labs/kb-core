# Changelog

All notable changes to kb-core. One version covers every part ([versioning](docs/versioning.md)); each release
lists its changes per part, the ESP32 port and its driver pins. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Configurator: the *Check* panel is replaced by a *Supply chain* panel (`js/panel.js`, loaded after the first view):
  SPDX 2.3 and CycloneDX 1.6 downloads with component count, licences and pins; known vulnerabilities of the selected
  components from the weekly scan (none known, not scanned, missing data, a warning from the 11th day); the release
  asset and its `gh attestation verify` command; tests, coverage and MISRA chips per driver (kb-core's evidence until
  drivers have their own); real blockers next to their cause, warnings and notes under a closed *Details*. The
  stylesheet now uses `light-dark()` instead of a second copy of the dark palette.

### Changed

- Configurator: while kb-core has only release candidates the page starts on the newest one (the release-candidate
  switch turns on) instead of opening with an error; *Open an existing project* is folded (now inside *Advanced*);
  on small screens a bar at the bottom shows whether the selection resolves and offers the ZIP.

### Changed

- Figures: all 22 diagrams are drawn from figkit model files (`docs/model/<id>.<kind>.yml`) with the shared kinds
  (graph, state, seq, matrix, pipeline), about 640 px wide and read top to bottom so they stay readable on a phone;
  the components map still reads the repository folders, and the thread-model figure stays a module of plain figkit
  calls. The kb-core helper layer `tools/docs/kbfig.py` is removed, and `tools/docs/check_figures.py` accepts a model
  file or a module that draws with figkit ([Figures](docs/figures.md)).

### dma

- Board declarations (`kb/dma/assignment.hpp`): `StreamAssignment`, `InterruptLimits`, `BusLimits` and `StreamShape`,
  the values the board generator writes from `board.yml`, with the compile-time checks `RequireInterruptTier`,
  `RequireSamePriority`, `RequireBusCarries` and `RequireShape` and four compile-fail tests.
- The pinned copy a port keeps (`tools/layout/layout.py vendor`) now includes `components/lib/dma` (headers and the
  host model with the conformance suite), so a port runs the suite against its own boundary.

### Fixed

- ADR numbers: the asynchronous bus and held transactions record is ADR-0016 (it shared 0015 with the ThreadX
  backend); every reference follows.
- `components/lib/handlers/tests/test_threads.cpp`: a comment line over the 100-column limit.

## [0.1.0-rc.1] - 2026-11-21

The first release candidate: every part, the ESP32 and host ports and 23 pinned chip drivers, tested in CI. APIs
may still change before 0.1.0.

### Repository

- Layout ([Repository layout](docs/layout.md)): the libraries, the ports and the driver submodules sit next to each
  other under `components/` (`components/lib/`, `components/ports/`, `components/drivers/`); the JSON schemas live in
  `catalog/schemas/`. Where things live is named once, in `tools/layout/paths.yml`; CMake
  (`cmake/kb_layout.cmake`), the tools, `tools/preflight.sh` and the tests read it. `tools/layout/layout.py root`
  fails on a top-level entry outside the allowlist, and `layout.py stale` fails on a reference to a retired location.
  Consumers that keep a pinned copy of kb-core (`port-esp32`, `port-stm32`, `third_party/kb-core.lock`) get the list
  of copied files from `layout.py vendor`.
- Examples are first-class (`examples/README.md`): `examples/examples.yml` indexes them; host examples
  (`utils_checks`, `rtos_thread`, `interfaces_fakes`) are built with `KB_BUILD_EXAMPLES` and run as ctest tests
  `example_<id>` on every pull request; the documentation shows their source through snippet includes
  (`docs/parts/*.md`); the configurator catalogue carries the index (`examples`). `tools/examples/examples.py check`
  keeps all of it in step.
- Figures: every diagram is a committed light and dark SVG drawn by the Kokeb figure kit, readable at phone width;
  no Mermaid ([Figures](docs/figures.md)).

### dma

- New part `kb::dma` (`components/lib/dma/`, depends on utils): `Region` tags, `Buffer<Region, Bytes>` (static,
  line-aligned, padded), move-only `Lease` with Cpu / Device / Quarantined states, port cache hooks (`CacheOps`), the
  exactly-once `CompletionLatch`, `Stream<Frame, Frames, Region>` (copy-out ring with sequence numbers, timestamps,
  three overrun checks, signed stall check, content check, restart policy and flagged fallback), `Capabilities` and
  `DmaHealth`, compile-time frame, reach and alignment checks.
- Host model of caches and a DMA engine with injectable defects, and the conformance suite T1 to T18, each case shown
  to fail on the model with its defect; compile-fail tests, a stream fuzz target, a threaded test and a mutation
  catalogue (`tools/mutants/dma.txt`).
- ADR-0030 to ADR-0032 (numbers provisional).

### Build, CI and tools

- Test infrastructure: a dependency-free harness (`tests/harness/`), strict flags, header self-containment checks,
  compile-fail and fuzz helpers (`cmake/kb_testing.cmake`); `kb_parts_cross` for the Cortex-M build and scan.
- `tools/preflight.sh`: every CI check that runs on a workstation, plus the Linux gates and the MISRA scan in the
  CI images; `tools/lint/` holds misra_lint, eval_order_check and recursion_check.
- CI: the standalone, integration, mutation (level high, enforced), pin hygiene, layering and catalogue callers run
  on ci-tools v1.19; the MISRA scan and quality gates cover the library code (`lib/<part>/inc`, boundary files),
  not tests.
- Doxygen API reference (`_config/Doxyfile`), published under `/api/`.
- ThreadX (ADR-0015): the official kernel as the pinned submodule `third_party/threadx`; CI builds the ThreadX
  backend on it for three Cortex-M cores (`ci-cross-build.yml`) and runs its Linux-port tests in the host job; pin
  hygiene ignores `third_party/**` (ci-tools 1.21 `ignore_paths`). `tools/preflight.sh` takes `JOBS` to cap its
  builds, tests and clang-tidy, and runs the ThreadX Linux-port tests at 1000 and 100 Hz.

- Repository skeleton: layout, build settings, documentation skeleton, ADR-0001 to ADR-0005, CI caller stubs.
- Design and plan in the site (`docs/design/`), figure sources in `docs/diagrams/` embedded in the pages; every
  page in the navigation, strict build clean.
- ADR-0006: interface contracts are C++20 concepts only, with a register of permitted CRTP bases; supersedes
  ADR-0003 and records the relation to handbook ADR-0016.
- ADR-0007 and ADR-0008: MCU ports isolated in `ports/<mcu>/` as their own CMake projects; names of the kb
  repository family (`kb-port-<family>`, `kb-board-<board>`, `kb-configurator`, ...).
- ADR-0009 and ADR-0010: handler thread models declared per bus; start-up as a pipeline of declared stages,
  including bno08x and se05x.
- ADR-0011: assurance level `high` (was `safety`); drivers and handlers are not the safety layer. CI callers,
  `.kokeb/handbook.yml` and docs updated.
- ADR-0012: pins to release or rc tags only; release lock with kb-core's own SHA; pin checks PIN-011, PIN-012.
- ADR-0013: hosted configurator with repeatable downloads and evidence; skeleton page in `configurator/`.
- Install package: `kbConfig.cmake` loads component targets bottom-up, so `find_package(kb COMPONENTS interfaces)`
  and `handlers` work from an installed tree (checked with the ESP32 port's seam configure).
- Include checker rules for the port seam (LAY-010, LAY-011), no `virtual` (LAY-012) and the symbol lint.
- ADR-0014: kb-core is the assembler: its own libraries plus a catalogue and lock of pinned parts, versions
  selectable per driver, one resolver specification (`docs/design/assembler.md`) for the CLI and the site.
- ADR-0008 amended: MCU port repositories are brandless, `kokeb-labs-drivers/port-<family>` (`port-esp32`,
  `port-stm32`); code names unchanged. ADR-0001, ADR-0002, ADR-0004 and ADR-0005 accepted.
- JSON schemas in `schemas/`: catalogue, `kb-image.yml`, lock (image lock and ADR-0012 release lock).
- `kb` tool (`tools/kb/`): `kb catalog build` (offline from clones, online with `gh`), `kb resolve` (lock,
  `CMakePresets.json`, `kb_image.cmake`, a fetch script that checks every commit), `kb sbom` (CycloneDX 1.6,
  SPDX 2.3, deterministic); unit tests, a seeded property test, CTest `kb_cli_unit`.
- `configurator/resolver.mjs`: the site's resolver, the same rules; shared vectors in `tests/resolver/vectors/`
  run by both (CTest `kb_resolver_js_vectors`) plus a Python-JavaScript agreement test on mutated images.
- One project layout: `kb-fetch.sh` adds kb-core, the port and the drivers as submodules under `external/` at their
  tags (moved tags refused, `--verify` mode); `kb_image.cmake` and the presets set `KB_DRIVER_<NAME>_DIR`.
- `kb sbom` lists ESP-IDF, FreeRTOS and ThreadX as SOUP and each component's `kb:osv` scan (`--catalog`); the
  catalogue carries optional `osv`, kb-core `bindings` and driver `requires_owner`/`bindings`
  (`schemas/driver-yml.schema.json`, `kb catalog build --platforms`).
- Configurator (ADR-0013, P1 and P2 of the site): the page in `configurator/` (platform, RTOS and versions,
  searchable driver catalogue with chip figures, buses and thread models), its site checks (`CFG-*`,
  `docs/configurator-checks.md`) next to the shared rules of `resolver.mjs`, and the downloads: `kb-image.yml`, the
  lock, CMake files and `kb-fetch.sh` from the shared resolver, CycloneDX and SPDX SBOMs with SOUP and scans,
  licence and status reports, a starter project archive; `offline.html` for use from disk. Catalogue built from the
  drivers' tags (`configurator/tools/build_catalog.py`), every script pinned by SRI. User guide:
  `docs/configurator.md`.
- CI: `ci-configurator.yml` runs the site's tests, the headless browser check when Chrome is present, and the
  catalogue, SRI and offline-bundle freshness checks. The catalogue check leaves out a kb-core tag on the checked
  commit or a later one, so a release tag's own CI run passes; each kb-core release lists its handler bindings
  (the headers in `components/lib/handlers/inc/kb/handlers/bindings/` at that tag).
- Update path: `kb outdated` and `kb upgrade [--to NAME=VERSION] [--allow-rc]` plan an upgrade of a project
  from its `kb-image.yml` and lock (kb-core first, drivers gated against it, held-back versions with the gate's
  code, SemVer breaking changes, known vulnerabilities fixed and remaining) and write the new image, lock, CMake
  files, `kb-fetch.sh` and `kb-upgrade.md`; codes `KB-UPG-*`. The configurator's *Open an existing project* reads
  the files locally and shows the same plan with the same downloads (`plan` in `resolver.mjs`). Shared vectors in
  `tests/resolver/vectors/upgrade/`, Python-JavaScript agreement tests on mutated inputs, a YAML-reader parity test.
- Update path checks: a lock whose commit or kind differs from the catalogue is shown and warned
  (`KB-UPG-W04`); the site's YAML reader is linear with line caps, refuses `__proto__`, non-string keys and the
  plain forms PyYAML reads as numbers or dates; held-back versions cannot be picked and a refused pick keeps the
  table; release candidates are hidden behind a *Show release candidates* switch; "request it" and footer links to
  `kokeb-labs-drivers/driver-requests`; per-version `requires_owner` in the site catalogue and `kb` reads the
  site's `data/catalog.json` as the page does (a wrong catalogue is exit 2); the kb tool tests fail instead of
  skipping in CI without PyYAML.
- ADR-0016 (proposed): non-blocking bus access (`Start`/`Poll`, ports may adapt synchronous calls for now) and held
  bus transactions; design pages [Bus access](docs/design/bus-access.md) and
  [Fault evidence](docs/design/fault-journal.md) with their figures.

### utils

- `kb::Status` and `kb::Result<T>` (with `Result<void>`): one error model with a vendor detail code; factories in
  the value-initialise-then-set form; a default or `Failure(Ok)` result never reads as success.
- Typed time: `kb::Microseconds`, `kb::Milliseconds`, the 64-bit `kb::Instant`, saturating and round-up
  conversions, `MillisecondsToTicksCeil`, wrap-safe `ElapsedSince`/`HasElapsed`, `NarrowUs`/`NarrowMs`, `Deadline`.
- Checked, saturating and range-checked integer arithmetic (`numeric.hpp`), free of undefined behaviour.
- Constexpr table-driven CRCs with 11 catalogue presets (`crc.hpp`); byte-order loads and stores and the bounded
  `ByteReader`/`ByteWriter` (`endian.hpp`).
- `StaticVector`, `RingBuffer`, wait-free `SpscRing`; span helpers with `ConstantTimeEqual`; `FixedString` with
  number formatting and no printf.
- Unit tests, compile-fail tests, three libFuzzer targets with replayed seed corpora; ESP32-C6 smoke app.
- 100 % line and branch coverage; mutation kill rate 100 % (367 killed, 47 equivalents listed with reasons in
  `docs/assurance/equivalent-mutants.yml`); inputs go through `kb_test::Rt` so constexpr code runs at run time.
- `FaultJournal<Store, Slots>` (`fault_journal.hpp`): fault evidence that survives a reset. 32-byte records with a
  magic, a version and a CRC-32 in a ring over a `JournalStore` (`RamJournalStore<Bytes>` for host tests and RAM
  kept across a reset); torn writes detected at `Open()`, wrap-around with modulo-2^32 sequences, one bounded store
  write per `Append()`, the reset-reason record, `FindNewest(kind)`. Unit tests against a golden record and a
  libFuzzer target (`fuzz_utils_journal`) with seeds.

### rtos

- The typed API (header-only, RTOS-free): `SleepFor` (milliseconds or microseconds, at least the time asked),
  `Periodic` (fixed grid, overruns reported), `Now()`, `Mutex` with bounded `TryLockFor` and `LockGuard`,
  counting `Semaphore`, `Queue<T, N>` over internal storage, `EventFlags` (24 flags), `Timer`,
  `CriticalSection`, and the CRTP `Thread<Derived, StackBytes>` with a static stack, fixed-rate loop, stop handling
  and step statistics. Every object is constexpr-constructed and created explicitly; ISR-safe calls are the
  `...FromIsr` ones.
- Backends: `none` (deterministic fake clock, timers fire as time advances), `freertos` (two boundary files, static
  allocation only, built into the ESP-IDF component `kb`), the test-only `host` backend over std::thread in
  `components/ports/host/`.
- One contract test suite over `none` and over the FreeRTOS boundary with a fake kernel (`tests/fakes/freertos/`),
  FreeRTOS link-seam tests, multi-threaded host tests, compile-fail tests; ESP32-C6 smoke app `kb_rtos_smoke`.
- ThreadX backend (ADR-0015): `KB_RTOS=THREADX` over Eclipse ThreadX, one boundary file
  (`components/lib/rtos/backends/threadx/threadx_boundary.cpp`), static allocation only, priority-inheriting mutexes,
  the semaphore maximum, odd-sized queue items, delay-until with overrun count, nested critical sections and the stack
  high-water mark; the kernel is a product's (`KB_THREADX_INCLUDE_DIRS`) or the pinned submodule `third_party/threadx`
  (`v6.5.2.202603b_rel`, MIT, SOUP) built for `KB_THREADX_PORT`; the configure stops without one. Tested over a fake
  kernel (`tests/fakes/threadx/`, the contract suite and the seam), on the real kernel's Linux port
  (`KB_THREADX_HOST_TESTS`: the threaded suite and priority inheritance) and cross-compiled with the kernel for
  Cortex-M0+, M4F and M7F (`tools/preflight/threadx_cross.sh`, CI job ThreadX Cortex-M). Docs:
  `docs/parts/rtos-threadx.md`. The catalogue, the configurator and `kb resolve` list ThreadX as supported.
  `KB_THREADX_TICK_HZ` is the tick the kernel and the backend are built for (required with a product kernel; the backend
  fails to compile when the kernel's `TX_TIMER_TICKS_PER_SECOND` differs); `KB_THREADX_THREAD_BYTES` optionally checks
  that the backend sees the product kernel's control-block layout. kb threads never run as urgently as the ThreadX timer
  thread, and a delay-until that wakes after its grid point counts as an overrun.
- The seam and the API gain what the ThreadX backend needed and every backend now implements:
  `RecursiveMutex`/`RecursiveLockGuard`, `Thread::StartAndWaitToVerify`, `RequestPark`/`Resume`/`ResumeFromIsr`,
  `Thread::StackFreeBytes()` (FreeRTOS: the task high-water mark), `Semaphore::IsCreated()`; on a ThreadX build queue
  slots are whole kernel words (`KB_THREADX_QUEUE_WORD_BYTES`), elsewhere the item size. `Thread::Join` and
  `StartAndWaitToVerify` wait on the monotonic clock, ending within a tick of their timeout at any tick rate (`Join`
  overran by up to a tick per millisecond before). The delay-until, retake and tick-extension rules live in the
  platform-free `kb/rtos/backend_rules.hpp`.
- Fixed: the FreeRTOS backend created timers with the period in milliseconds as ticks (wrong unless the tick is
  1000 Hz); it now converts with rounding up.

### interfaces

- The contracts as C++20 concepts in `kb::iface`: `I2cBus`, `SpiDevice` (+ `SpiClockControl`), `UartPort`
  (+ `UartBaudControl`, `UartTxDrain`), `GpioOutput`, `GpioInput`, `GpioInterrupt`, `AdcChannel`, `PwmOutput`,
  `CanBus`, `MonotonicClock`, `BusyWait`, `LogSink`; vocabulary `Level`, `Edge`, `I2cFailure`, `CanFrame`,
  `LogLevel`, `Duty`.
- A type-erased `*Ref` per contract (context pointer and constant function table), each checked against its concept.
- Host port `kb::interfaces_host`: fakes of every contract with fault injection.
- Proof against the pinned drivers tmc5160, bno08x and pcal9555: thin adapters satisfy
  their port concepts over the fakes and over the `*Ref` values; compile-fail tests for misuse.
- The asynchronous contract (`async.hpp`, ADR-0016): `I2cAsync`, `SpiAsync`, `AsyncOperation`, `NonBlocking`,
  `AsyncPoll`; `AsyncFromSync<Bus, Clock>` serves it from a synchronous port with the same deadline rules. Host
  fakes `FakeAsyncI2c` and `FakeAsyncSpi` inject latency, hangs, late completions, NACKs and the cancel race.


### handlers

- Handler core (`kb/handlers/common/`): `HandlerCore<Derived, Driver, Snapshot, Command, Reply>` (CRTP) with the
  start-up pipeline runner over a declared `kStartupPlan` (sync and polled stages, deadlines from configuration,
  optional stages, `StartupReport`, retry from the first stage), the health record (Unknown, Starting, Ready,
  Degraded, Faulted, Recovering; counters, last error with the driver's code, last good time; a caller's
  `InvalidArgument` is refused, not counted), `Recover()` with backoff and an attempt limit, a published snapshot
  and a liveness beat. `EnsureInitialized()` is the one start; calls never start or recover a chip.
- `ServiceKey`: every mutating call needs it, and only the facades can make one.
- `BusDomain<Bus, ThreadModel::Owner>` (service pass with an owner check) and `BusDomain<Bus, ThreadModel::Guarded>`
  (bus mutex, statistics, bus recovery), `BusHold`; `OwnerAccess<H>` (lock-free `Latest()`, bounded request queue
  `RequestChannel`, start and recovery on the owner task) and `Guarded<H>` (handler lock then bus lock, bounded),
  with one caller API; a wrong facade, a wrong bus kind or `Guarded` over a `kRequiresOwner` chip does not compile.
- `Seqlock<T>`, `LivenessMonitor<N>`, the hook bases `I2cHooks`, `SpiHooks`, `UartHooks`, `AdcHooks`, `GpioHooks`
  and `HookTiming` (the at-least delay split, clock narrowing), and the driver-result helpers.
- One binding and handler per driver (`kb/handlers/bindings/<name>.hpp`) for all 22 pinned drivers and for se05x
  (se05x included): port over kb contracts, the error map, the
  start-up plan, the thread model and a small typed app API. One page per handler under `docs/handlers/`.
- Tests: unit tests of the core on the none backend, thread tests on the host backend (contention, timeouts,
  recovery storms), compile-fail tests, a fuzz target of the request queue, integration tests of every binding
  against the driver's own chip model in both thread models (`tests/integration/`, `KB_BUILD_INTEGRATION`), and a
  mutation catalogue of the core (`tools/mutants/handlers.txt`).
- CI: the MISRA scan covers the handler core (the bindings need the driver submodules, which the scan does not
  check out yet: they are scanned locally); coverage, lizard and the standalone matrix cover `kb::handlers`; the
  layering check compiles the integration tests; the catalogue records each binding.
  `tools/preflight/misra.sh` no longer reports a failure when the scan has no blocking finding.
- Configurator: canopen_client recorded as requiring an owner task; the catalogue shows the new driver pins.
- `BusDomain::Hold(timeout)` returns a `[[nodiscard]]`, move-only `BusHold` on both models (the bus mutex, or the
  owner pass flag, which a service pass then cannot take); `Release()`, `Bus()`, `IsOn()`, owner hold statistics.
  `BusHold` is no longer built directly. `Guarded<H>::Hold(timeout)` (`GuardedHeld`: handler then bus lock) and
  `OwnerAccess<H>::Held(hold)` (`OwnerHeld`: inline calls on the owner task) run a sequence of calls whole; the
  at25040b app calls run under either, with an integration test of a record written whole against another writer.
  Compile-fail tests: a copied hold, a discarded hold, the bus of a temporary hold, another bus's hold, a guarded
  `Held`, a blocking hot path.
- `AsyncPoller<Op, Bytes, Use>`: one asynchronous transfer per owner pass with its own buffers, never waiting;
  `AsyncUse::NonBlocking` (default) refuses a port whose Start() waits.
- Liveness: `LivenessBeat::Park()`, `Resume()`, `Parked()`; `LivenessMonitor::Verdict()` (stale and parked bits,
  `kick` only when something is watched and nothing is stale).
- `HealthJournal<Sink, N>`: the health path into the fault journal (entering Faulted, serving again, new liveness
  stalls), retried when the sink refuses.
- Tests: unit tests of each, thread tests of held calls under contention (TSan), mutation catalogue entries
  `BH-*`, `GH-*`, `OH-*`, `AP-*`, `LV-02` to `LV-04`, `HJ-*`.

### ports

- `components/ports/esp32/`: the ESP32 family port, its own CMake project and ESP-IDF component with its own deviation
  files and ESP-IDF examples. `components/ports/host/`: the host port (RTOS backend over std::thread and fakes of
  every contract) used by the tests.

### Driver pins

Submodules under `components/drivers/`, recorded in `components/drivers/drivers.lock.yml`; `catalog/compat.yml`
generated from them.

- se05x at v0.1.0-rc.2.
- At v0.1.0-rc.1: ads7952, ads9324, alicat_basis2, as5047u, at25040b, bmp581, bno08x, canopen_client, dynamixel,
  fdo2, isf15acp4, max22200, mcp9700, ntc_thermistor, pca9685, pcal9555, pf1550, sfm_flow_meter, tle92466ed,
  tmc5160, tmc9660 and ws2812_rmt.

[Unreleased]: https://github.com/kokeb-labs/kb-core/compare/v0.1.0-rc.1...HEAD
[0.1.0-rc.1]: https://github.com/kokeb-labs/kb-core/releases/tag/v0.1.0-rc.1
