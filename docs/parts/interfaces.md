# kb::interfaces

The contracts of buses, pins, clocks and logs, as C++20 concepts in `kb::iface` and only concepts (ADR-0006), the
vocabulary types they use, a type-erased `*Ref` for each, and the host test port. Used only by the HAL,
`kb::handlers` and the MCU ports; applications never include it.

```cpp
#include "kb/interfaces/interfaces.hpp"
```

## The contracts

| Concept | Header | Members (all `noexcept`) | `*Ref` |
| --- | --- | --- | --- |
| `I2cBus` | `i2c.hpp` | `Status EnsureReady()`, `Status Write(addr, span<const u8>)`, `Status WriteRead(addr, tx, rx)`, `Status Read(addr, span<u8>)`, `I2cFailure LastFailure() const` | `I2cBusRef` |
| `SpiDevice` | `spi.hpp` | `Status EnsureReady()`, `Status Transfer(span<const u8> tx, span<u8> rx)` (CS held) | `SpiDeviceRef` |
| `SpiClockControl` | `spi.hpp` | `SpiDevice` + `Status SetClockHz(u32)` | - |
| `UartPort` | `uart.hpp` | `Status EnsureReady()`, `Status Write(span<const u8>)`, `Result<size_t> Read(span<u8>, Milliseconds)`, `Status FlushInput()` | `UartPortRef` |
| `UartBaudControl`, `UartTxDrain` | `uart.hpp` | `Status SetBaud(u32)`; `Status WaitTxDone(Milliseconds)` | - |
| `GpioOutput` | `gpio.hpp` | `Status Set(Level)`, `Status Toggle()` | `GpioOutputRef` |
| `GpioInput` | `gpio.hpp` | `Status Get(Level&)` | `GpioInputRef` |
| `GpioInterrupt` | `gpio.hpp` | `GpioInput` + `Status EnableInterrupt(Edge, GpioCallback, void*)`, `Status DisableInterrupt()` | - |
| `AdcChannel` | `adc.hpp` | `Result<u32> ReadRaw()`, `Result<i32> ReadMillivolts()`, `u32 FullScale() const` | `AdcChannelRef` |
| `PwmOutput` | `pwm.hpp` | `Status SetFrequencyHz(u32)`, `Status SetDuty(Duty)`, `Status Enable(bool)` | `PwmOutputRef` |
| `CanBus` | `can.hpp` | `Status EnsureReady()`, `Status Send(const CanFrame&, Milliseconds)`, `Status Receive(CanFrame&, Milliseconds)` | `CanBusRef` |
| `MonotonicClock` | `time.hpp` | `kb::Instant Now() const` (64-bit microseconds) | `MonotonicClockRef` |
| `BusyWait` | `time.hpp` | `void WaitUs(Microseconds)` (below 1 ms; longer waits use `kb::rtos::SleepFor`) | `BusyWaitRef` |
| `LogSink` | `log.hpp` | `void Write(LogLevel, std::string_view)` (bounded, no allocation) | `LogSinkRef` |
| `I2cAsync`, `SpiAsync` | `async.hpp` | the sync contract + `Status Start...(..., Deadline)`, `AsyncPoll Poll(Instant)`, `Status Cancel()`, `kNonBlocking`; `AsyncFromSync<Bus, Clock>` adapts a sync port ([bus access](../design/bus-access.md), ADR-0016) | - |

Every `*Ref` is in `refs.hpp` (or the umbrella `interfaces.hpp`).

Vocabulary (`types.hpp`): `Level`, `Edge`, `I2cFailure` (`None`, `AddressNack`, `DataNack`, `Bus`, `Timeout`,
`BusStuck`), `CanFrame`, `LogLevel`, `Duty` (0.01 % steps). Results are `kb::Status` or `kb::Result<T>`; a port puts
its vendor code in `Detail()`.

## A port

```cpp
class Esp32I2cBus {                                   // in components/ports/esp32 (its own project)
public:
  kb::Status EnsureReady() noexcept;
  kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept;
  kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept;
  kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept;
  kb::iface::I2cFailure LastFailure() const noexcept;
};
static_assert(kb::iface::I2cBus<Esp32I2cBus>);        // beside the definition, always
```

## Run-time choice without virtual functions

A `*Ref` is a context pointer and a pointer to a constant table of function pointers, made from any type that
satisfies the concept and itself checked against it. It costs one indirect call per operation:

```cpp
kb::iface::GpioOutputRef reset_line = board.UsesExpander() ? kb::iface::GpioOutputRef{expander_pin3}
                                                           : kb::iface::GpioOutputRef{mcu_pin12};
```

## Serving the drivers' ports

Every driver keeps its own CRTP port base and concept. A binding derives from the driver's base and forwards each
hook to a kb contract in a line or two; the tests prove it for three drivers over the host fakes and over the
`*Ref` values (`components/lib/interfaces/tests/driver_ports/`):

| Driver port | Adapter over | Notes |
| --- | --- | --- |
| `tmc5160::SpiPort` | `SpiDevice` | `Transfer(tx, rx, n)` is one `Transfer(span, span)` |
| `tmc5160::UartPort` | `UartPort` | `Exchange` is `FlushInput`, `Write`, `Read(rx, timeout)`; a short read is `Timeout` |
| `bno08x::HubPort` (I2C) | `I2cBus`, `GpioInput`, `MonotonicClock` | `IntAsserted` reads H_INTN low; `NowUs` narrows with `kb::utils::NarrowUs` |
| `bno08x::HubPort` (SPI) | `SpiDevice`, `GpioOutput`, `GpioInput`, `MonotonicClock` | `SetWake(true)` drives WAKE low |
| `pcal9555::I2cPort` | `I2cBus`, `LogSink` | `GetLastFailure` maps `I2cFailure`; the optional `Log` hook maps levels |

```cpp
template <kb::iface::SpiDevice Dev>
class Tmc5160Spi : public tmc5160::SpiInterface<Tmc5160Spi<Dev>> {
public:
  explicit Tmc5160Spi(Dev& dev) noexcept : dev_(dev) {}
  bool EnsureInitialized() noexcept { return dev_.EnsureReady() == kb::Status::Ok; }
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t n) noexcept {
    return dev_.Transfer({tx, n}, {rx, n}) == kb::Status::Ok;
  }
private:
  Dev& dev_;
};
static_assert(tmc5160::SpiPort<Tmc5160Spi<kb::iface::SpiDeviceRef>>);
```

## The host port

`components/lib/interfaces/host/` (`kb::interfaces_host`, header-only, test code outside the MISRA claim): `FakeI2cBus`
(register-file devices, failure injection per kind), `FakeSpiDevice` (scripted replies, transmit log),
`FakeUart` (receive script, responder, transmit log), `FakeGpioPin` (output, input and edge interrupts),
`FakeAdc`, `FakePwm`, `FakeCanBus` (bus-off), `FakeClock` (jumps across 2^32 microseconds), `FakeBusyWait`,
`FakeLogSink`, and `FakeAsyncI2c`, `FakeAsyncSpi` (the asynchronous contract over the I2C and SPI fakes: latency in
polls, a hang, a late completion, the cancel race). Each is `static_assert`ed against its concept.

## MISRA C++:2023 notes

- The contract headers: zero findings and zero deviations (level `high`). The `*Ref` values live in `refs.hpp`,
  kb-core's one library deviation, DEV-KBC-004 (Rule 8.2.6): one helper, `detail::ContextAs<T>`, turns the `void*`
  context back into the type its table was built for (ADR-0011 amendment).
- Concepts only, no virtual function (ADR-0006); the function tables are namespace-scope structs and variable
  templates, so no type is nested in a class template; comparison operators are non-members.

## Example

[`examples/host/interfaces_fakes/`](../examples.md) runs a function written to the `I2cBus` concept against the host
port's fake bus; the same function takes a target port's bus unchanged. It is built and run by CI.

```cpp
--8<-- "examples/host/interfaces_fakes/main.cpp:checks"
```

## Tests

`test_contracts.cpp` (every fake through its `*Ref`, every failure kind), `test_async.cpp` (the asynchronous
contract, `AsyncFromSync` and the asynchronous fakes with every fault), `compile_fail/` (a `*Ref` from a
non-conforming type, a member that is not `noexcept`, a wrong result type, an unbounded UART read, a 32-bit clock)
and `driver_ports/` (the five adapters above, at compile time and at run time; built when the driver submodules
are checked out).
