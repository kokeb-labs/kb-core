# kb::utils

Vocabulary types and algorithms with no OS, no MCU and no I/O: the error model (`kb::Status`, `kb::Result<T>`),
strong time units with round-up conversions, wrap-safe elapsed time, fixed-capacity containers, an ISR-safe
single-producer single-consumer ring, constexpr CRCs, byte packing, saturating and range-checked arithmetic and
heap-free text formatting.

Rules:

- Depends on nothing but the handbook's target library subset. Applications may use it directly.
- No platform macro is ever tested here; the poison build (`tests/poison/`) proves it.
- Every public function has unit tests in `tests/`; every parser or container with untrusted input has a fuzz
  target in `fuzz/` with its seed corpus in `fuzz/corpus/` (the `.bin` files there are tracked).
- Header-only: the implementation is in the headers (templates and constexpr functions); `src/` stays empty.

Status: M1 implemented. Guide: `docs/parts/utils.md`; API: `_config/Doxyfile`.
