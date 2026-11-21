# kb::utils

Vocabulary types and algorithms with no OS, no MCU and no I/O: the one error model, typed time, fixed-capacity
containers, CRCs, byte packing, checked arithmetic and heap-free text. Header-only, constexpr where it can be,
depends on nothing. Applications, the HAL, handlers and ports may all use it.

```cpp
#include "kb/utils/utils.hpp"   // or one header per area, below
```

| Header | Gives | Contract in one line |
| --- | --- | --- |
| `status.hpp` | `kb::Status`, `kb::ToString()` | `Ok` is the only success; every name is a literal, never null |
| `result.hpp` | `kb::Result<T>`, `kb::Result<void>` | A value or a failure with a vendor `Detail()`; a default result is a failure |
| `time.hpp` | `kb::Microseconds`, `kb::Milliseconds`, `kb::Instant`, `ElapsedSince()`, `Deadline` | Distinct duration types; elapsed time by unsigned subtraction; instants are never ordered |
| `numeric.hpp` | `CheckedAdd/Sub/Mul`, `SaturatingAdd/Sub/Mul`, `InRangeCast`, `Saturate`, `Clamp`, `DivCeil` | Exact or refused, never undefined; no `bool` or character types |
| `crc.hpp` | `Crc<T, Poly, Init, Reflected, XorOut>` and 11 presets | Table built at compile time; each preset proven on its catalogue check value |
| `endian.hpp` | `LoadBe16 ... StoreLe64`, `ByteReader`, `ByteWriter` | Fixed-extent spans; a cursor fails sticky at the first overrun |
| `static_vector.hpp` | `StaticVector<T, N>` | Inline storage; a call that does not fit returns `false` and changes nothing |
| `ring_buffer.hpp` | `RingBuffer<T, N>`, `SpscRing<T, N>` | One-context FIFO; wait-free single-producer single-consumer FIFO (N a power of two) |
| `span.hpp` | `CheckedSubspan`, `CopyInto`, `Fill`, `Equal`, `ConstantTimeEqual`, `ObjectBytes` | No offset arithmetic can wrap; secret comparison in constant time |
| `fixed_string.hpp` | `FixedString<N>` | NUL-terminated, truncation flagged, numbers formatted without printf |
| `fault_journal.hpp` | `FaultJournal<Store, Slots>`, `RamJournalStore<Bytes>`, `JournalStore`, `FaultRecord`, `ResetReason` | 32-byte CRC-checked records in a ring that survives a reset; torn writes detected; one bounded write per append ([fault evidence](../design/fault-journal.md)) |

## The error model

Every kb-core call that can fail returns `kb::Status` or `kb::Result<T>`. A result has no public data: it is made
by `Success(value)` or `Failure(status, detail)` and read by `IsOk()`, `Value()`, `ValueOr()`, `Error()` and
`Detail()`. Ports put the vendor error code in `detail`, so bench debugging keeps it.

```cpp
kb::Result<uint16_t> ReadId(Bus& bus) noexcept {
  uint16_t raw = 0U;
  if (!bus.Read(raw)) {
    return kb::Result<uint16_t>::Failure(kb::Status::BusError, bus.VendorCode());
  }
  return kb::Result<uint16_t>::Success(raw);
}

const auto id = ReadId(bus);
if (!id.IsOk()) {
  log.Write(kb::ToString(id.Error()));
}
```

`Failure(Status::Ok)` is a programming error and is recorded as `Status::Fault`, so a failure never reads as a
success.

## Time without wrap defects

```cpp
const kb::Instant start = clock.Now();                       // 64-bit microseconds
const auto limit = kb::utils::Deadline::After(start, kb::Milliseconds{20U});
while (!limit.Expired(clock.Now())) { ... }

// A driver hook with a 32-bit millisecond clock:
const uint32_t now_ms = kb::utils::NarrowMs(clock.Now());
if (kb::utils::HasElapsed(now_ms, since_ms, 100U)) { ... }   // right across the 2^32 wrap
```

- `Milliseconds` and `Microseconds` do not convert into each other implicitly, and a bare integer is not a
  duration (compile-fail tests prove both).
- `ToMicroseconds` saturates, `ToMillisecondsCeil` and `MillisecondsToTicksCeil` round up: a wait converted with
  them is never shorter than asked.
- There is no `operator<` on `Instant`. Comparing two timestamps is the half-range defect; compare the elapsed time
  with the timeout instead.

## Frames and checksums

```cpp
std::array<uint8_t, 8> frame{};
kb::utils::ByteWriter w{frame};
const bool built = w.WriteU8(0x01U) && w.WriteBe16(raw) && w.WriteBe32(serial);
const uint8_t crc = kb::utils::Crc8Smbus::Compute(w.Written());

kb::utils::ByteReader r{reply};
uint16_t value = 0U;
const bool parsed = r.Skip(1U) && r.ReadLe16(value) && r.AtEnd();
```

| Preset | Poly | Init | Reflected | XorOut | Check |
| --- | --- | --- | --- | --- | --- |
| `Crc8Smbus` | 0x07 | 0x00 | no | 0x00 | 0xF4 |
| `Crc8Sensirion` | 0x31 | 0xFF | no | 0x00 | 0xF7 |
| `Crc8Maxim` | 0x31 | 0x00 | yes | 0x00 | 0xA1 |
| `Crc8SaeJ1850` | 0x1D | 0xFF | no | 0xFF | 0x4B |
| `Crc16CcittFalse` | 0x1021 | 0xFFFF | no | 0x0000 | 0x29B1 |
| `Crc16Xmodem` | 0x1021 | 0x0000 | no | 0x0000 | 0x31C3 |
| `Crc16Kermit` | 0x1021 | 0x0000 | yes | 0x0000 | 0x2189 |
| `Crc16Modbus` | 0x8005 | 0xFFFF | yes | 0x0000 | 0x4B37 |
| `Crc32Ieee` | 0x04C11DB7 | 0xFFFFFFFF | yes | 0xFFFFFFFF | 0xCBF43926 |
| `Crc32C` | 0x1EDC6F41 | 0xFFFFFFFF | yes | 0xFFFFFFFF | 0xE3069283 |
| `Crc32Mpeg2` | 0x04C11DB7 | 0xFFFFFFFF | no | 0x00000000 | 0x0376E6E7 |

## Containers

```cpp
kb::utils::RingBuffer<Sample, 16> history{};
static_cast<void>(history.PushOverwrite(sample));   // keeps the newest 16

kb::utils::SpscRing<uint8_t, 64> rx{};              // ISR produces, task consumes
// ISR:  if (!rx.TryPush(byte)) { ++overruns; }
// task: uint8_t b = 0U; while (rx.TryPop(b)) { Parse(b); }
```

Elements are trivially copyable value types; storage is inline, so a container is as large as its capacity and
never allocates.

## MISRA C++:2023 notes

- Zero findings and zero deviations is the target for every header here (level `high`, ADR-0011).
- Factories build a value-initialised object and set its fields (`Result r{}; r.error_ = e; return r;`), the form
  the scan accepts under Rule 0.2.2.
- Comparison operators are non-member functions (Rule 16.6.1); there are no user-defined literals (Rule 6.9.2) and
  no types nested in class templates.
- Every `if ... else if` ends in `else`; operands of `&&` and `||` are parenthesised (Rule 8.0.1); arithmetic on
  narrow types is done after an explicit cast to `uint32_t` or the widest type (Rules 7.0.4 to 7.0.6).
- Digits are looked up in a table, never computed from a character (Rule 7.0.3); no `printf`, so no `PRId64` on
  newlib.

## Example

[`examples/host/utils_checks/`](../examples.md) runs on a PC and is built and run by CI; its source is shown here
by a snippet include, so this page cannot drift from code that compiles.

```cpp
--8<-- "examples/host/utils_checks/main.cpp:checks"
```

## Tests

`components/lib/utils/tests/`: unit tests of every function at its limits, constexpr `static_assert` tests, and
compile-fail
tests (`compile_fail/`) for misuse the types reject. `components/lib/utils/fuzz/`: libFuzzer targets for the CRCs
(against a
bitwise reference), the byte reader and writer, and the containers (against an array model); their seed corpora
replay as CTests on every compiler.
