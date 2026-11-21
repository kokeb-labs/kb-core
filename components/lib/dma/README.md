# kb::dma

Buffers that a DMA master reads or writes, and the streams built on them (docs: `docs/parts/dma.md`, decisions:
ADR-0030 to ADR-0032). Header-only, depends on `kb::utils`.

- `inc/kb/dma/`: `Region` tags (cache policy, line size, alignment unit, reachable masters), `Buffer<Region, Bytes>`
  and the move-only `Lease` with its Cpu / Device / Quarantined states, the port's cache hooks (`CacheOps`), the
  exactly-once `CompletionLatch`, `Stream<Frame, Frames, Region>` (copy-out ring, overrun and stall checks,
  supervision), `Capabilities` and `DmaHealth`, compile-time frame, reach and alignment checks.
- `host/`: a host model of caches and a DMA engine with injectable defects, a reference port, and the conformance
  suite T1 to T18 (`kb::dma_host`, test code).
- `tests/`: unit tests, the conformance suite against the good model and a broken model per case, a threaded stream
  test and compile-fail tests; `fuzz/`: the stream's ring against a model.
- `src/`: empty; the part is header-only.
