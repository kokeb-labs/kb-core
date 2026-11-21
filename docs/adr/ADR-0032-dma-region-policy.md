# ADR-0032: A region states its cache policy, and the default is non-cacheable

Status: Accepted (owner decision, 2026-11-15). The number is provisional until the record is merged.

## Context

Which memory a DMA master may use, and what the cache does to it, differs per part: tightly coupled memory that the
master cannot reach, cached SRAM that needs maintenance by line, SRAM in a domain next to the peripheral. A buffer
placed in the wrong one fails only on the board.

## Decision

1. A `Region` states its cache policy (`Coherent`, `NonCacheable`, `Maintained`), its line size, its alignment unit
   and the masters that reach it. The unit is a power of two at least as large as a line; a `Maintained` region has
   a real line size.
2. The default DMA region on a part with a data cache is a non-cacheable block in the memory domain next to the
   peripheral, set up by the MPU. `Maintained` regions are for buffers that must live in cached memory.
3. Compile-time helpers fail the build when a frame exceeds a port's limit, a master cannot reach a region, a size is
   not a whole number of units, or a stream element differs from the board declaration.
4. The start-up check `Buffer::Verify` confirms alignment and placement inside the region's range.

## Consequences

- No cache maintenance in the common path; the maintained path stays tested by the model.
- A misplaced buffer or oversized frame is a build error, not a field failure.

## Alternatives considered

- Cache maintenance everywhere: correct, but every missing call is a silent stale read.
- One global policy: wrong for parts with more than one memory.
