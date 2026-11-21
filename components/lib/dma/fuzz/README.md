# components/lib/dma/fuzz/

`fuzz_dma_stream`: any sequence of publishes (clean or torn), hardware overruns, takes, gives, stall checks and
supervision against `Stream`. Checked: sequence numbers strictly increase, a skipped number is flagged `Gap()`, a
held frame never changes, an overflow is never a frame, and the counters add up (`published`, `overruns`,
`delivered`). `corpus/fuzz_dma_stream/` holds the seed inputs, replayed as a CTest on every compiler.
