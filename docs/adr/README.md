# Architecture decision records

Decisions of kb-core, one file each, numbered in order and never reused. A decision is changed by a new record
that supersedes or amends the old one; the old record stays. Handbook decisions are cited as "handbook ADR-NNNN".

| ADR | Decision | Status |
| --- | --- | --- |
| [ADR-0001](ADR-0001-layering.md) | Layering rule and its enforcement | Accepted; rule 2 amended by ADR-0007 |
| [ADR-0002](ADR-0002-driver-consumption.md) | Drivers are consumed as submodules pinned to release tags | Accepted; sharpened by ADR-0012, amended by ADR-0014 |
| [ADR-0003](ADR-0003-concepts-vs-crtp.md) | Concepts for contracts, CRTP only for shared behaviour | Superseded by ADR-0006 |
| [ADR-0004](ADR-0004-ci-strategy.md) | CI: what kb-core runs and what it trusts from driver CI | Accepted; level set by ADR-0011 |
| [ADR-0005](ADR-0005-no-code-port.md) | No code is ported from older stacks | Accepted |
| [ADR-0006](ADR-0006-concepts-only.md) | Interface contracts are C++20 concepts only; a register of permitted bases | Accepted |
| [ADR-0007](ADR-0007-mcu-ports-isolated.md) | MCU ports start isolated in `components/ports/<mcu>/`, ready to move out | Accepted |
| [ADR-0008](ADR-0008-repository-naming.md) | Repository names of the kb family | Accepted; amended 2026-11-14 (brandless `port-<family>`) |
| [ADR-0009](ADR-0009-handler-thread-models.md) | Handler thread models, declared per bus | Accepted |
| [ADR-0010](ADR-0010-handler-startup-pipeline.md) | Handler start-up is a pipeline of declared stages | Accepted |
| [ADR-0011](ADR-0011-assurance-level-high.md) | Assurance level `high`; drivers and handlers are not the safety layer | Accepted |
| [ADR-0012](ADR-0012-pins-and-release-lock.md) | Pins to release or rc tags only; release lock with its own SHA | Accepted; amended by ADR-0014 |
| [ADR-0013](ADR-0013-hosted-configurator.md) | Hosted configurator with repeatable, pinned projects and evidence | Accepted; amended by ADR-0014 |
| [ADR-0014](ADR-0014-kb-core-is-the-assembler.md) | kb-core is the assembler: own libraries plus a manifest and lock of pinned parts, one resolver | Accepted |
| [ADR-0015](ADR-0015-threadx-backend.md) | Eclipse ThreadX is a supported kb::rtos backend, its kernel a pinned SOUP submodule | Accepted |
| [ADR-0016](ADR-0016-async-bus-and-held-transactions.md) | Non-blocking bus access (`Start`/`Poll`) and held bus transactions (`BusHold`) | Proposed |
| [ADR-0017](ADR-0017-native-object-lifetime.md) | A `kb::rtos` object's destructor deletes its native object (one delete call per kind in the backend seam) | Accepted |
| [ADR-0030](ADR-0030-dma-buffers-and-leases.md) | DMA buffers are owned by the CPU or the device and move by a lease; a cancel that cannot be confirmed quarantines (number provisional) | Accepted |
| [ADR-0031](ADR-0031-dma-stream-copy-out.md) | Streams copy frames out of the DMA ring and say what they lost (number provisional) | Accepted |
| [ADR-0032](ADR-0032-dma-region-policy.md) | A region states its cache policy; the default is non-cacheable (number provisional) | Accepted |
| [ADR-0033](ADR-0033-dma-resources-declared-once.md) | DMA resources are declared once in `board.yml` and checked before hardware (number provisional) | Accepted |

Format: Status, Context, Decision, Consequences, Alternatives considered.
