# Port contracts

kb contracts are C++20 concepts on the kb side, and only concepts ([ADR-0006](adr/ADR-0006-concepts-only.md)).
Each driver keeps its own port concept, and a binding in `kb::handlers` satisfies the driver's port over kb
contracts. MCU ports (`components/ports/<mcu>/`) and the host test port satisfy the kb contracts.

## What the drivers ask for

Every driver port is a CRTP base `<chip>::<Bus>Interface<Derived>` plus a concept that checks each hook's exact
signature. All hooks are `noexcept`; every driver requires `bool EnsureInitialized()`.

| Bus | Required hooks in drivers | Optional hooks | Drivers |
| --- | --- | --- | --- |
| I2C | `Write(addr, data, len)`, `WriteRead(addr, tx, n, rx, m)`, some `Read(addr, ...)` | `GetLastFailure(I2cFailure&)`, `DelayUs`, `GpioSet`/`GpioGet`, `Log` | pf1550, pca9685, pcal9555, bmp581, sfm-flow-meter, se05x, bno08x |
| SPI | `Transfer(tx, rx, len)`, full duplex, CS held | `DelayUs`, `GpioSet`/`GpioGet`/`GpioRead`, `SckHz`, `Log` | ads7952, ads9324, as5047u, at25040b, bmp581, isf15acp4, max22200, tle92466ed, tmc5160, tmc9660, bno08x |
| UART | `Write(data, len)`, `Read(data, len, timeout)`, `FlushInput()`; single-wire `Exchange` | `SetBaud`, `DelayMs`/`DelayUs`, `NowMs`/`NowUs`, `Log`, RS-485 direction pin | alicat-basis2, dynamixel, fdo2, tmc5160, tmc9660, bno08x |
| ADC | `ReadVoltageMv(ch, mv&)` or `ReadRaw(ch, raw&)` | `DelayUs`, `GpioSet` | mcp9700, ntc-thermistor |
| CAN | `Send(frame)`, `Receive(frame&, timeout)` | `NowUs` | canopen-client |
| PIO | `Transmit(span, waveform, timeout)`, `ResolutionHz(hz&)` | `DelayUs`, `GpioSet` | ws2812-rmt |
| Time | `NowMs()`, `NowUs()` (32 or 64 bit), `DelayUs(us)`, `DelayMs(ms)` | - | most |
| Pins | `GpioSet(CtrlPin, GpioSignal)`, `GpioGet(...)`, `SetReset(bool)`, `SetWake`, `IntAsserted` (bno08x), `Reset`, `PowerCycle`, `SetEnable` (se05x) | - | many |

## kb contracts

The concepts, their members and their `*Ref` values are listed on the [interfaces page](parts/interfaces.md),
with the adapters that prove they serve the drivers' ports.

| Concept | Requires |
| --- | --- |
| `kb::iface::I2cBus` | `EnsureReady`, `Write`, `WriteRead`, `Read` (spans), `LastFailure` (`None`, `AddressNack`, `DataNack`, `Bus`, `Timeout`, `BusStuck`) |
| `kb::iface::SpiDevice` | `EnsureReady`, `Transfer(tx, rx)` with CS held; optional `SpiClockControl` |
| `kb::iface::UartPort` | `EnsureReady`, `Write`, `Read(span, timeout)` returning a count, `FlushInput`; optional `UartBaudControl`, `UartTxDrain` |
| `kb::iface::GpioOutput`, `GpioInput`, `GpioInterrupt` | `Set(Level)`, `Toggle()`; `Get(Level&)`; edge interrupts with a callback |
| `kb::iface::AdcChannel` | `ReadRaw`, `ReadMillivolts`, `FullScale` |
| `kb::iface::PwmOutput` | `SetFrequencyHz`, `SetDuty(Duty)`, `Enable` |
| `kb::iface::CanBus` | `EnsureReady`, `Send(frame, timeout)`, `Receive(frame, timeout)` |
| `kb::iface::MonotonicClock` | `Now()` as the 64-bit `kb::Instant`; bindings narrow with wrap-safe arithmetic |
| `kb::iface::BusyWait` | `WaitUs` below 1 ms; longer waits use `kb::rtos::SleepFor` |
| `kb::iface::LogSink` | `Write(level, text)`, bounded, no allocation |

The asynchronous forms `I2cAsync` and `SpiAsync` (`async.hpp`, ADR-0016) are the sync contracts plus
`Start...(deadline)`, `Poll(now)` and `Cancel()`; a port without a native form serves them through
`AsyncFromSync`. Planned: `PulseTx` (ws2812-rmt).

## Interface rules

| Rule | Detail |
| --- | --- |
| Concepts only | No base class carries a contract; each port has `static_assert(Concept<Port>)` beside it; compile-fail tests reject incomplete types |
| Run-time choice | Through a type-erased `*Ref` (context pointer plus a constant table of function pointers), itself checked against the concept; added per contract when a HAL needs one |
| Bus and device are separate | A controller per peripheral (owns recovery and statistics) and a small device value (controller reference, address, clock, timeout) that satisfies the contract; no container of owned devices, no integer handles |
| Every call is bounded | Blocking calls take a timeout or use the device's; `RecoverBus()` (nine clocks and a STOP) is explicit |
| Results | `kb::Result<std::size_t>` for transfers, `kb::Status` for the rest; `detail` holds the vendor code |
| Held transaction | `BusDomain::Hold(timeout)` returns an RAII `BusHold`; a driver call, or a sequence of calls through `Guarded::Hold()` or `OwnerAccess::Held()`, runs inside it (two-frame reads, EEPROM ranges, se05x T=1); see [bus access](design/bus-access.md) |
| Asynchronous form | `I2cAsync`, `SpiAsync`: `Start(..., deadline)`, `Poll(now)` never waits, `Cancel()`; completion from the ISR by an event flag; a late completion is `Timeout` |
| Buffers | `std::span` in kb contracts; bindings adapt to the drivers' pointer-plus-length hooks |

The bindings bridge the drivers' differences: start-up names (the [start-up pipeline](design/handlers.md#start-up-pipeline)),
read results (`bool`, `size_t`, `BusRead{status,count}`, `BusStatus`, `RxStatus`), four clock shapes, three I2C
shapes, pin hooks and two log shapes. The hook bases absorb the common cases; bno08x and se05x get hand-written
bindings. No driver has or needs a lock hook: locking is the access facade's job
([thread models](design/handlers.md#thread-models)).

The se05x driver also needs host cryptography (random, hash, AES, ECDH, ECDSA); the HAL supplies the driver's
crypto port directly, and kb-core defines no crypto contract.
