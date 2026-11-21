# Figures

<!-- Written by kokeb-docs figures from docs/diagrams/figures/*.py. Do not edit by hand. -->

Every figure on this site is drawn by a Python module from facts extracted from the code, so it is rebuilt,
not redrawn, when the code changes. CI runs `kokeb-docs figures --check` and fails when a
committed figure no longer matches the code. Each figure below lists the files it is drawn from.

## From tags to a project

<!-- figure: assembler-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assembler-flow.dark.svg">
  <img src="assembler-flow.svg"
    alt="Driver and kb-core tags feed kb catalog build, which writes catalog.json; the resolver reads it and
    kb-image.yml and writes the lock and the project files; kb sbom turns the lock into SBOMs.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/assembler-flow.py`](../../docs/diagrams/figures/assembler-flow.py)

## One asynchronous read

<!-- figure: async-poll -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="async-poll.dark.svg">
  <img src="async-poll.svg"
    alt="Sequence: the owner task starts a read through the poller and the port in pass n; the poller polls
    and gets Pending; the interrupt completes the transfer; in pass n+1 the poll returns Done with the
    byte count and the completion is Ok. Past the deadline the result is Timeout; Cancel releases the
    buffers.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/async-poll.py`](../../docs/diagrams/figures/async-poll.py)

## Held transactions

<!-- figure: bus-hold -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="bus-hold.dark.svg">
  <img src="bus-hold.svg"
    alt="Two side by side models. Guarded bus: task X takes the handler mutex, then the bus mutex, and makes
    several calls while task Y's plain request waits or times out. Owner bus: the owner task takes the
    hold between passes and runs inline calls; the service pass reports Busy while the hold is held.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/bus-hold.py`](../../docs/diagrams/figures/bus-hold.py)

## One call through the layers

<!-- figure: call-sequence -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="call-sequence.dark.svg">
  <img src="call-sequence.svg"
    alt="Sequence: the HAL calls a guarded handler, which locks and calls the pf1550 driver; the driver calls
    the kb binding's WriteRead, which calls the ESP32 I2C device; results return the same way. Only the
    I2C device knows it is an ESP32.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/call-sequence.py`](../../docs/diagrams/figures/call-sequence.py)

## The CI flow

<!-- figure: ci-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="ci-flow.dark.svg">
  <img src="ci-flow.svg"
    alt="Flow: local preflight, then a pull request; always-run checks, path-gated checks and the drivers
    that are never re-run feed one required gate; a tag then produces the release assets and a schedule
    re-scans them.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/ci-flow.py`](../../docs/diagrams/figures/ci-flow.py)

## Parts, ports and drivers

<!-- figure: components-map -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="components-map.dark.svg">
  <img src="components-map.svg"
    alt="Map of the repository: the lib parts utils, rtos, interfaces and handlers, each depending on the
    next down; MCU ports that satisfy the interface contracts; and the pinned drivers that handlers
    bind.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/components-map.py`](../../docs/diagrams/figures/components-map.py)

Drawn from:

- [`components/lib`](../../components/lib)
- [`components/ports`](../../components/ports)
- [`components/drivers`](../../components/drivers)

## Which layer may depend on which

<!-- figure: dependency-rules -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="dependency-rules.dark.svg">
  <img src="dependency-rules.svg"
    alt="Two lists of dependency arrows between layers. Allowed: twelve, from the application down to
    kb::utils. Forbidden: seven, each labelled with the check that catches it.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/dependency-rules.py`](../../docs/diagrams/figures/dependency-rules.py)

## Ownership of a DMA buffer

<!-- figure: dma-lease -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="dma-lease.dark.svg">
  <img src="dma-lease.svg"
    alt="State machine with the states Cpu, Device and Quarantined: Lend moves a buffer from Cpu to Device,
    Complete or a proven Abort returns it to Cpu, an unproven Abort moves it to Quarantined, and Recover
    with proof returns it to Cpu.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/dma-lease.py`](../../docs/diagrams/figures/dma-lease.py)

## The DMA copy-out stream

<!-- figure: dma-stream -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="dma-stream.dark.svg">
  <img src="dma-stream.svg"
    alt="Flow: the DMA ring raises an interrupt that copies each finished frame into a slot; the task takes a
    slot as a FrameView, a content check feeds the supervisor, which can switch to a declared fallback
    producer; a full ring drops the frame and flags the next one with Gap.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/dma-stream.py`](../../docs/diagrams/figures/dma-stream.py)

## Fault evidence path

<!-- figure: fault-journal -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="fault-journal.dark.svg">
  <img src="fault-journal.svg"
    alt="Flow: handler health edges and the liveness verdict go to the health journal, which appends one
    record to a CRC-checked ring in a port-provided store that survives a reset; the boot reset reason
    is recorded too; the watchdog is kicked only when nothing is stale; after a reset Open finds the
    newest record.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/fault-journal.py`](../../docs/diagrams/figures/fault-journal.py)

## Generators and the configurator

<!-- figure: generator-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="generator-flow.dark.svg">
  <img src="generator-flow.svg"
    alt="Three columns. Inputs: driver.yml files, the release lock, evidence, tested version sets and later
    board.yml. Middle: the kb command line and the configurator site, both using one resolver. Outputs:
    kb-image.yml, git commands, CMake presets, evidence for the selection and later a project archive.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/generator-flow.py`](../../docs/diagrams/figures/generator-flow.py)

## Handler lifecycle and health

<!-- figure: handler-health -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-health.dark.svg">
  <img src="handler-health.svg"
    alt="State machine: Unknown to Starting to Ready; Ready and Degraded swap on a failing and a passing
    call; Starting, Degraded and Ready can go to Faulted; Faulted goes to Recovering and back to Ready
    or Faulted; Deinitialize returns Ready or Faulted to Unknown.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/handler-health.py`](../../docs/diagrams/figures/handler-health.py)

## The pieces of a handler

<!-- figure: handler-pieces -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-pieces.dark.svg">
  <img src="handler-pieces.svg"
    alt="Stack: a HAL manager calls an access facade, OwnerAccess or Guarded; the facade calls the handler
    core with a service key and owns the bus domain; the core holds the start-up runner, health,
    snapshot and liveness beat and builds the driver, which reaches the HAL's bus through the binding's
    port and the kb::iface contract.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/handler-pieces.py`](../../docs/diagrams/figures/handler-pieces.py)

## The handler start-up pipeline

<!-- figure: handler-startup -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-startup.dark.svg">
  <img src="handler-startup.svg"
    alt="Pipeline of eight start-up stages in order, Power, HostReset, Boot, Link, Identify, Secure,
    Configure and Verify, ending in Ready; beside it the plans of most drivers, the bno08x and the
    se05x, each using only some of the stages.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/handler-startup.py`](../../docs/diagrams/figures/handler-startup.py)

## Handler thread models

<!-- figure: handler-threading -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-threading.dark.svg">
  <img src="handler-threading.svg"
    alt="Two side by side models. Owner task: other tasks read a lock-free snapshot or post requests to a
    queue that the owner task publishes to and drains, and the owner task steps the handlers on an
    unlocked bus domain. Guarded facade: tasks call a guarded handler that takes the handler lock then a
    bounded bus lock.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/handler-threading.py`](../../docs/diagrams/figures/handler-threading.py)

## The layer stack

<!-- figure: layer-stack -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="layer-stack.dark.svg">
  <img src="layer-stack.svg"
    alt="Layer stack, top to bottom: the product application; the board HAL repository; kb-core with
    kb::handlers, kb::interfaces, kb::rtos and kb::utils, each depending on the next; below it the
    drivers used by handlers, the MCU ports that satisfy the interface contracts, and the vendor SOUP
    reached only through boundary files.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/layer-stack.py`](../../docs/diagrams/figures/layer-stack.py)

## MISRA C++:2023 scope

<!-- figure: misra-scope -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="misra-scope.dark.svg">
  <img src="misra-scope.svg"
    alt="Four zones from strongest claim to none: kb-core own code claimed compliant with zero deviations;
    boundary files compliant with recorded deviations; drivers covered by their own evidence; SOUP out
    of scope and never reported.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/misra-scope.py`](../../docs/diagrams/figures/misra-scope.py)

## The kb repository family

<!-- figure: repo-family -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="repo-family.dark.svg">
  <img src="repo-family.svg"
    alt="Repository graph with consumers on top: the product application uses a product board HAL, which uses
    kb-hal-common and an MCU port; both depend on kb-core, which pins the drivers. The configurator
    reads kb-core releases and fills the application template; tested version sets list sets of kb-core.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/repo-family.py`](../../docs/diagrams/figures/repo-family.py)

## Phase roadmap

<!-- figure: roadmap -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="roadmap.dark.svg">
  <img src="roadmap.svg"
    alt="Two rows of four phases, M0 to M7, in order: decisions, utils, rtos, interfaces, ESP32 port and
    bindings, handler rollout, breadth and generators, each with its exit criteria.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/roadmap.py`](../../docs/diagrams/figures/roadmap.py)

## kb::rtos backends

<!-- figure: rtos-backends -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rtos-backends.dark.svg">
  <img src="rtos-backends.svg"
    alt="The kb::rtos API reaches one backend through a single seam: none, FreeRTOS, host (tests only) or
    ThreadX; FreeRTOS and ThreadX reach their kernel, which is SOUP, through boundary files and are
    tested on fake kernels.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/rtos-backends.py`](../../docs/diagrams/figures/rtos-backends.py)

## The ThreadX backend

<!-- figure: rtos-threadx -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rtos-threadx.dark.svg">
  <img src="rtos-threadx.svg"
    alt="The kb::rtos API and seam lead to two ThreadX backend files; the boundary file reaches the ThreadX
    kernel, which comes either from a product's copy or the pinned submodule; tests run it on a fake
    kernel and on the Linux port.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/rtos-threadx.py`](../../docs/diagrams/figures/rtos-threadx.py)

## The update path

<!-- figure: update-path -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="update-path.dark.svg">
  <img src="update-path.svg"
    alt="Five steps: read the lock, catalogue and image; order kb-core, port and drivers; apply the driver
    gates and hold back what fails; write one row per component; write the new image, lock, CMake files,
    fetch script and report, then review, fetch, build and test.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/update-path.py`](../../docs/diagrams/figures/update-path.py)
