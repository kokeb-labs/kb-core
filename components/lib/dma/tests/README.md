# components/lib/dma/tests/

- `test_vocab.cpp`: regions, rounding, the compile-time helpers, the cache hooks per policy, the buffer and lease
  state machine (lend, complete, abort with and without proof, quarantine, recovery, a destroyed lease), the
  completion latch, capabilities.
- `test_stream.cpp`: order, sequence numbers and times, held frames, the overrun checks, torn frames, the signed
  stall check, content judgement, restart, back-off and fallback, `PublishFromDevice`.
- `test_conformance.cpp`: the cases T1 to T18 each pass on the good model and fail on a model with its defect.
- `test_threads.cpp`: a producer and a consumer thread on one stream (run under ThreadSanitizer by the gates).
- `compile_fail/`: a copied lease, a forged key, a copied buffer, a zero-byte buffer, a bad region, a master that
  cannot reach the region, an oversized frame, a size that is not whole units, an element size mismatch, a one-frame
  stream, a non-trivial frame, a movable stream, a copied view and a cache hook that lacks a member do not compile.
