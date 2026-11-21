# components/lib/dma/inc/kb/dma/

Public headers of `kb::dma`, included as `#include "kb/dma/<name>.hpp"` or all at once with `dma.hpp`.

| Header | Content |
| --- | --- |
| `region.hpp` | `Region` concept, `CachePolicy`, `Direction`, `MasterSet`, compile-time `Require*` checks |
| `cache.hpp` | `CacheOps` (the port's clean, invalidate, barrier) and `NoCacheOps` |
| `completion.hpp` | `Outcome`, `StopEvidence`, `CompletionLatch` (completion and cancel exactly once) |
| `buffer.hpp` | `Buffer`, `Lease`, `PortKey`, `BufferState`, `BufferStats` |
| `stream.hpp` | `Stream`, `StreamConfig`, `StreamStats`, `SignedElapsedUs`, `SampleInstant` |
| `capabilities.hpp` | `TransferMode`, `Capabilities`, `DmaHealth` |
