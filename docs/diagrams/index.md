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

Hardware description: [`docs/model/assembler-flow.graph.yml`](../model/assembler-flow.graph.yml)

Drawn from:

- [`docs/model/assembler-flow.graph.yml`](../model/assembler-flow.graph.yml)

## One asynchronous read

<!-- figure: async-poll -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="async-poll.dark.svg">
  <img src="async-poll.svg"
    alt="Sequence in which the owner task starts a read through the poller and the port in pass n, polls and
    gets Pending, the interrupt completes the transfer, and in pass n+1 the poll returns Done with the
    byte count.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/async-poll.seq.yml`](../model/async-poll.seq.yml)

Drawn from:

- [`docs/model/async-poll.seq.yml`](../model/async-poll.seq.yml)

## Held transactions

<!-- figure: bus-hold -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="bus-hold.dark.svg">
  <img src="bus-hold.svg"
    alt="Two chains side by side: on a guarded bus task X takes the handler mutex and then the bus mutex
    while task Y waits; on an owner bus the owner task takes the hold between passes and the service
    pass reports Busy.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/bus-hold.graph.yml`](../model/bus-hold.graph.yml)

Drawn from:

- [`docs/model/bus-hold.graph.yml`](../model/bus-hold.graph.yml)

## One call through the layers

<!-- figure: call-sequence -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="call-sequence.dark.svg">
  <img src="call-sequence.svg"
    alt="Call chain in which the HAL calls a guarded handler, which locks and calls the pf1550 driver, the
    driver calls the kb binding's WriteRead, which calls the ESP32 I2C device, and the results return
    the same way.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/call-sequence.graph.yml`](../model/call-sequence.graph.yml)

Drawn from:

- [`docs/model/call-sequence.graph.yml`](../model/call-sequence.graph.yml)

## The CI flow

<!-- figure: ci-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="ci-flow.dark.svg">
  <img src="ci-flow.svg"
    alt="Flow from the local preflight to a pull request, then always-run and path-gated checks into one
    required gate, with the drivers' own tests never re-run, and a tag producing the release assets that
    a schedule re-scans.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/ci-flow.graph.yml`](../model/ci-flow.graph.yml)

Drawn from:

- [`docs/model/ci-flow.graph.yml`](../model/ci-flow.graph.yml)

## Parts, ports and drivers

<!-- figure: components-map -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="components-map.dark.svg">
  <img src="components-map.svg"
    alt="Map of the repository in which the lib parts handlers, interfaces, rtos and utils each rest on the
    next one down, the MCU ports satisfy the interface contracts, and handlers bind the pinned drivers.">
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
    alt="Table of nineteen dependency rules between layers, twelve allowed from the application down to
    kb::utils and seven forbidden, each forbidden one with the check that catches it.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/dependency-rules.matrix.yml`](../model/dependency-rules.matrix.yml)

Drawn from:

- [`docs/model/dependency-rules.matrix.yml`](../model/dependency-rules.matrix.yml)

## Ownership of a DMA buffer

<!-- figure: dma-lease -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="dma-lease.dark.svg">
  <img src="dma-lease.svg"
    alt="State machine with the states Cpu, Device and Quarantined: Lend moves a buffer to Device, Complete
    or a proven Abort returns it to Cpu, an unproven Abort quarantines it, and Recover with proof
    returns it to Cpu.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/dma-lease.state.yml`](../model/dma-lease.state.yml)

Drawn from:

- [`docs/model/dma-lease.state.yml`](../model/dma-lease.state.yml)

## The DMA copy-out stream

<!-- figure: dma-stream -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="dma-stream.dark.svg">
  <img src="dma-stream.svg"
    alt="Flow in which the DMA ring raises an interrupt that copies each finished frame into a slot, the task
    takes a slot as a FrameView, a content check feeds the supervisor, and a full ring drops the frame
    and flags the next with Gap.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/dma-stream.graph.yml`](../model/dma-stream.graph.yml)

Drawn from:

- [`docs/model/dma-stream.graph.yml`](../model/dma-stream.graph.yml)

## Fault evidence path

<!-- figure: fault-journal -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="fault-journal.dark.svg">
  <img src="fault-journal.svg"
    alt="Flow in which handler health edges and the liveness verdict go to the health journal, which appends
    one record to a CRC-checked ring in a port store that survives a reset, with the boot reset reason
    recorded and the watchdog kicked only when nothing is stale.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/fault-journal.graph.yml`](../model/fault-journal.graph.yml)

Drawn from:

- [`docs/model/fault-journal.graph.yml`](../model/fault-journal.graph.yml)

## Generators and the configurator

<!-- figure: generator-flow -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="generator-flow.dark.svg">
  <img src="generator-flow.svg"
    alt="Flow from the release inputs through the kb command line or the configurator site to one resolver,
    which writes kb-image.yml, git commands, CMake presets, evidence for the selection and later a
    project archive.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/generator-flow.graph.yml`](../model/generator-flow.graph.yml)

Drawn from:

- [`docs/model/generator-flow.graph.yml`](../model/generator-flow.graph.yml)

## Handler lifecycle and health

<!-- figure: handler-health -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-health.dark.svg">
  <img src="handler-health.svg"
    alt="State machine of a handler with the states Unknown, Starting, Ready, Degraded, Faulted and
    Recovering, and the calls and failures that move it between them.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/handler-health.state.yml`](../model/handler-health.state.yml)

Drawn from:

- [`docs/model/handler-health.state.yml`](../model/handler-health.state.yml)

## The pieces of a handler

<!-- figure: handler-pieces -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-pieces.dark.svg">
  <img src="handler-pieces.svg"
    alt="Stack in which a HAL manager calls an access facade, the facade runs the handler core with a service
    key and owns the bus domain, and the core builds the driver, which reaches the HAL's bus through the
    binding's port and the kb::iface contract.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/handler-pieces.graph.yml`](../model/handler-pieces.graph.yml)

Drawn from:

- [`docs/model/handler-pieces.graph.yml`](../model/handler-pieces.graph.yml)

## The handler start-up pipeline

<!-- figure: handler-startup -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-startup.dark.svg">
  <img src="handler-startup.svg"
    alt="Eight start-up stages in order, Power, HostReset, Boot, Link, Identify, Secure, Configure and
    Verify, ending in Ready, beside the plans of most drivers, the bno08x and the se05x, each using only
    some of the stages.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/handler-startup.pipeline.yml`](../model/handler-startup.pipeline.yml)

Drawn from:

- [`docs/model/handler-startup.pipeline.yml`](../model/handler-startup.pipeline.yml)

## Handler thread models

<!-- figure: handler-threading -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="handler-threading.dark.svg">
  <img src="handler-threading.svg"
    alt="Two thread models, one above the other: in model O other tasks read a lock-free snapshot or queue
    requests that the owner task publishes and drains, and in model G tasks call guarded handlers that
    take the handler lock and then a bounded bus lock.">
</picture>
<!-- /figure -->

Figure module: [`docs/diagrams/figures/handler-threading.py`](../../docs/diagrams/figures/handler-threading.py)

## The layer stack

<!-- figure: layer-stack -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="layer-stack.dark.svg">
  <img src="layer-stack.svg"
    alt="Layer stack from the product application through the board HAL to kb::handlers, kb::interfaces,
    kb::rtos and kb::utils, with the drivers that handlers bind, the MCU ports that satisfy the
    contracts, and vendor SOUP reached only through boundary files.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/layer-stack.graph.yml`](../model/layer-stack.graph.yml)

Drawn from:

- [`docs/model/layer-stack.graph.yml`](../model/layer-stack.graph.yml)

## MISRA C++:2023 scope

<!-- figure: misra-scope -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="misra-scope.dark.svg">
  <img src="misra-scope.svg"
    alt="Table of four zones from strongest claim to none, with kb-core's own code claimed compliant with
    zero deviations, boundary files compliant with recorded deviations, drivers covered by their own
    evidence and SOUP out of scope.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/misra-scope.matrix.yml`](../model/misra-scope.matrix.yml)

Drawn from:

- [`docs/model/misra-scope.matrix.yml`](../model/misra-scope.matrix.yml)

## The kb repository family

<!-- figure: repo-family -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="repo-family.dark.svg">
  <img src="repo-family.svg"
    alt="Repository graph with consumers on top, in which the product application uses a board HAL built on
    kb-hal-common and an MCU port, both resting on kb-core, which pins the drivers, while planned tool
    repositories read kb-core.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/repo-family.graph.yml`](../model/repo-family.graph.yml)

Drawn from:

- [`docs/model/repo-family.graph.yml`](../model/repo-family.graph.yml)

## Phase roadmap

<!-- figure: roadmap -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="roadmap.dark.svg">
  <img src="roadmap.svg"
    alt="Column of eight phases in order, M0 to M7: decisions, utils, rtos, interfaces, the ESP32 port and
    bindings, the handler rollout, breadth and generators, each with its exit criteria.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/roadmap.graph.yml`](../model/roadmap.graph.yml)

Drawn from:

- [`docs/model/roadmap.graph.yml`](../model/roadmap.graph.yml)

## kb::rtos backends

<!-- figure: rtos-backends -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rtos-backends.dark.svg">
  <img src="rtos-backends.svg"
    alt="The kb::rtos API reaches one backend through a single seam, none, FreeRTOS, ThreadX or host for
    tests, and FreeRTOS and ThreadX reach their kernel, which is SOUP, through boundary files and are
    tested on fake kernels.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/rtos-backends.graph.yml`](../model/rtos-backends.graph.yml)

Drawn from:

- [`docs/model/rtos-backends.graph.yml`](../model/rtos-backends.graph.yml)

## The ThreadX backend

<!-- figure: rtos-threadx -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rtos-threadx.dark.svg">
  <img src="rtos-threadx.svg"
    alt="The kb::rtos API and seam lead to two ThreadX backend files, and the boundary file reaches the
    ThreadX kernel, from a product's copy or the pinned submodule, which tests run on a fake kernel and
    on the Linux port.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/rtos-threadx.graph.yml`](../model/rtos-threadx.graph.yml)

Drawn from:

- [`docs/model/rtos-threadx.graph.yml`](../model/rtos-threadx.graph.yml)

## The update path

<!-- figure: update-path -->
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="update-path.dark.svg">
  <img src="update-path.svg"
    alt="Five steps from reading the lock, catalogue and image through ordering, gating, one row per
    component and writing the new files, with failing versions held back and a final review, fetch,
    build and test by hand.">
</picture>
<!-- /figure -->

Hardware description: [`docs/model/update-path.graph.yml`](../model/update-path.graph.yml)

Drawn from:

- [`docs/model/update-path.graph.yml`](../model/update-path.graph.yml)
