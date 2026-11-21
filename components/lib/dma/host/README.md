# components/lib/dma/host/

The host model of the DMA hardware side (`kb::dma_host`, test code outside the MISRA claim), and the conformance
suite.

- `machine.hpp`: memory with a write-back cache (speculative refills), the CPU's loads and stores, cache
  maintenance by line, DMA reads and writes, and a log of violations (missing clean, stale read, lost write,
  unaligned maintenance, access during a lease, DMA after return, DMA outside the buffer).
- `engine.hpp`: one DMA channel with bursts, error injection, a stop that completes at the end of a burst, a stuck
  mode that never stops, and receive-length rounding.
- `model_port.hpp`: a reference port over the engine, written the way a real port must be, with a `Faults` struct
  that switches on each defect of the incidents (the broken models of the tests).
- `rig.hpp`, `stream_rig.hpp`: the rigs the cases drive (`HostRig`, `AdcRig<Stream>`), and `NaiveStream`, the
  stream of the earlier implementations.
- `conformance.hpp`: the cases T1 to T18, templated over a rig; a port's adapter provides the same member functions.
