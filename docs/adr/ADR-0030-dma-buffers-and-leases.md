# ADR-0030: DMA buffers are owned by the CPU or the device, and move by a lease

Status: Accepted (owner decision, 2026-11-15). The number is provisional until the record is merged.

## Context

The incidents behind this decision were all one mistake in different clothes: memory that a DMA master writes was
also touched by the CPU, or the cache was not told. A transmit buffer read stale because the clean was missing; a
receive buffer read stale because the invalidate ran only before the transfer; an invalidate over a range that shared
a line with a neighbouring object destroyed the neighbour; a transfer that timed out returned its buffer while the
engine was still writing; a synchronous call made the caller's own stack a DMA target. Each was fixed by a comment.

## Decision

1. A DMA buffer is `Buffer<Region, Bytes>`: static, aligned to the region's unit, padded to a whole number of
   units. Cache maintenance over it never reaches another object.
2. A buffer is `Cpu`, `Device` or `Quarantined`. `CpuView()` returns the data in `Cpu` only; elsewhere it is empty and
   counted.
3. Only the port moves a buffer, through a `PortKey<Tag>` that only the tag's class can construct. A lease is
   move-only. `Lend` runs the maintenance over the owned lines; `Complete` returns the memory.
4. **Staging copy is the default.** A port copies caller data into its own staging buffer and back. Zero-copy is
   possible only when the application holds a `Lease` of a buffer in a region the master reaches.
5. **`Cancel()` returns `Fault` and quarantines the buffer when a stop cannot be confirmed** (the enable bit does not
   read 0 within its bound). A destroyed active lease quarantines too. `Recover` returns the buffer only with proof
   after the peripheral was re-initialised.
6. A transfer completes once and is reported once (`CompletionLatch`): a cancel racing the completion produces one
   outcome and the memory is the CPU's afterwards.
7. A port reports `Capabilities()` (mode, ceiling, staging size, alignment) and a `DmaHealth` record with the mode
   in use. A fallback to interrupt or polled service is declared and visible, never silent.

## Consequences

- A misuse that the incidents show (CPU access during a lease, a forged key, a copied lease) is a build error or a
  counted refusal, not a comment.
- Each port writes the register code and the three cache hooks; the rules live once in `kb::dma`.
- A cancel can fail, and the caller must handle `Fault`; a quarantined buffer stays unusable until recovered.

## Alternatives considered

- Reference counting leases: hides the one moment that matters (the master stopped) behind a count.
- Zero-copy by default: the caller's memory becomes a DMA target; rejected after the synchronous-call incident.
- Returning the buffer at timeout and ignoring a late write: the late write is the incident.
