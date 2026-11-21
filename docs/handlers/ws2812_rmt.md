# ws2812_rmt handler

WS2812, SK6812 and compatible single-wire addressable LEDs, driven by a timed pulse peripheral (the ESP32 RMT
transmitter), through `kb::handlers::ws2812_rmt::Handler<Hw, Pixels>` (`kb/handlers/bindings/ws2812_rmt.hpp`, driver
`driver-ws2812-rmt` at the pin in `components/drivers/drivers.lock.yml`).

kb has no contract for a pulse peripheral (`BusKind::Pio`). `Hw` is the driver's own port type, supplied by the MCU
port: a class with `EnsureInitialized()`, `ResolutionHz()`, `Transmit()`, `DelayUs()` and optionally `GpioSet()`
(the strip supply) and `Log()`. The binding's `Port<Hw>` passes every hook through. The driver is MCU-gated to ESP32
in kb's build switch (`cmake/kb_mcu_gates.cmake`); the header still compiles anywhere, so the host tests run it over
the driver's own model of the channel.

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | none | An actuator: `Latest()` and `Sample()` return `NotReady` and `Unsupported` |
| Command | `SetPixel(io, index, colour, timeout, show)` | One pixel; with `show` the frame is sent too |
| Command | `FillStrip(io, colour, timeout, show)` | Every pixel; sends the frame by default |
| Command | `ClearStrip(io, timeout, show)` | Every pixel black; sends the frame by default |
| Command | `SetStripBrightness(io, brightness, timeout)` | Linear dimming of the next frames |
| Command | `Show(io, timeout)` | Send the frame |
| Command | `SetPower(io, on, timeout)` | The strip supply (needs `Hw::GpioSet`) |
| Command | `Command{Op::...}` with `Request()` | A range of pixels, read a pixel back; every reply carries the frame time |

`io` is either facade; the calls are the same on an owner bus and on a guarded bus.

## Thread model

`kRequiresOwner = false`, guarded by preference. A strip is updated sporadically by an application task, and its
channel is its own bus, so a guarded domain costs one uncontended lock per frame. An animation loop that sends frames
at a fixed rate can own the channel on an owner bus instead. `Show()` blocks for the frame time plus the port's
slack (`Config::chip.transmit_slack_ms`); request timeouts must cover it.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Link` | sync | `EnsureInitialized()`: configuration check, open the channel, resolve the waveform at its tick rate | `Config::link_deadline` |
| 1 | `Configure` (optional) | sync | `PowerOn()` when `Hw` has the pin, then a black frame (`Clear()`, `Show()`) | `Config::blank_deadline` |

Stage 1 runs when `Config::blank_on_start` is true, so the pixels hold a known state after a start. The supply pin
is the driver's own hook, so there is no separate `Power` stage. The pixels are write-only: there is no `Identify`.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `NotInitialized` | `NotReady` | Faulted at once |
| `NotPowered` | `NotReady` | Counts toward `fault_after` (a frame between `PowerOff` and `PowerOn`) |
| `Bus` | `BusError` | Counts toward `fault_after` (a hook failed, a frame was cut or timed out) |
| `TimingUnreachable`, `Unsupported` | `Unsupported` | Counts toward `fault_after`; at start-up the start fails |
| `InvalidArgument` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Timeout`, `DeviceIdMismatch`, `Crc`, `Busy`, `DeviceFault` | `Timeout`, `Fault`, `CrcMismatch`, `Busy`, `Fault` | Not returned by this driver |

`Deinitialize()` and recovery switch the supply off (`PowerOff()`) when `Hw` has the pin; recovery then runs both
stages again.

## Example

```cpp
using Strip = kb::handlers::ws2812_rmt::Handler<board::RmtPort, 60U>;    // board::RmtPort: the MCU port's channel
kb::handlers::BusDomain<board::RmtPort, kb::handlers::ThreadModel::Guarded> rmt0{rmt_port};
kb::handlers::Guarded<Strip, decltype(rmt0)> leds{rmt0, rmt_port, config};

static_cast<void>(rmt0.Create());
static_cast<void>(leds.Create());
if (leds.EnsureStarted(kb::Milliseconds{20U}).IsOk()) {
  const auto green = ws2812_rmt::Colour::FromPacked(0x002000U);
  static_cast<void>(kb::handlers::ws2812_rmt::FillStrip(leds, green, kb::Milliseconds{20U}));
}
```

Tests: `tests/integration/ws2812_rmt/` (the driver's RMT channel model as `Hw`, with and without the supply pin,
both thread models).
