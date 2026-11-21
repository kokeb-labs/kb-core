# isf15acp4 handler

ISF15ACP4-family OLED display pushbuttons (SSD1331-class controller over write-only SPI, 15 V OLED supply enable,
SPST switch), through `kb::handlers::isf15acp4::Handler<Dev>` (`kb/handlers/bindings/isf15acp4.hpp`, driver
`driver-isf15acp4` at the pin in `components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `last_event` (the newest switch event with its times), `events`, `presses`, `button_down`, `output`, `supply_on`, `scrolling`, `has_switch` |
| Command | `DisplayOn(io, timeout)`, `DisplayOff(io, timeout)` | Supply and panel on, or panel and supply off |
| Command | `Clear(io, color, timeout)` | The visible area to one colour |
| Command | `FillWindow(io, {x, y, width, height}, color, timeout)` | A window to one colour |
| Command | `DrawLine(io, {x0, y0, x1, y1}, color, timeout)` | A line with the controller's engine |
| Command | `SetContrast(io, a, b, c, timeout)` | The contrasts of the three colours |
| Command | `Command{Op::...}` with `Request()` | Dim, pixel, rectangle, filled rectangle, display mode, activity, raw switch read |

## Pins

| Index | Pin | Asserted | Polarity in `OutputPin` / `InputPin` |
| --- | --- | --- | --- |
| `kPinDc` | D/C# | data (high) | `active_low = false` |
| `kPinReset` | RES# | controller held in reset (low) | `active_low = true` |
| `kPinVcc` | OLED supply enable | supply on | as wired |
| `kPinSwitch` (input) | switch | the physical level | `active_low = false`; the driver applies `ButtonConfig::active_low` |

The chip select is the SPI device's. Without a switch input (or on the display-only variant) a sample reads the
driver state only.

## Thread model

`kRequiresOwner = false`: nothing arrives unasked. The switch is sampled by `Poll()`, which must run at least twice
per debounce time; on an owner bus the owner task does it every `sample_period` (10 ms or less for the default 20 ms
debounce) and other tasks read `Latest()`; on a guarded bus the task that wants the switch calls `Sample()` at that
rate. A full-frame upload (`Present()`) is not a command: the frame does not fit a request slot.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Identify + Configure` | sync | `EnsureInitialized()` (supply off, reset pulse, the whole configuration, RAM cleared), then `DisplayOn()` when `Config::display_on` | `Config::start_deadline` |

The driver drives RES# and the supply enable itself, so the plan has no `Power` or `HostReset` stage. The controller
cannot be read: the stage proves the transfers and pins worked, not that a controller answered.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `NotInitialized` | `NotReady` | Faulted at once |
| `SupplyOff` | `NotReady` | Counts toward `fault_after` |
| `Bus` | `BusError` | Counts toward `fault_after` (a transfer, a pin write or the switch read failed) |
| `InvalidArgument`, `ScrollNotSet` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `ScrollActive`, `Locked`, `Busy` | `Busy` | Counts toward `fault_after` |
| `Unsupported`, `Timeout`, `Crc`, `DeviceIdMismatch`, `DeviceFault` | `Unsupported`, `Timeout`, `CrcMismatch`, `Fault` | Counts toward `fault_after` |

Recovery is the generic plan: `Deinitialize()` (the power-down: display off, supply off, `vcc_off_ms`), then the
start-up again. The port's `DelayUs` hook goes through the hook
timing (a spin below 1 ms, a sleep from 1 ms), so the reset pulse and the engine settle time are at least as long as
the driver asks.

## Example

```cpp
using Dev = board::Spi1Cs2;                                       // a kb::iface::SpiDevice
using Key = kb::handlers::isf15acp4::Handler<Dev>;
kb::handlers::BusDomain<board::Spi1, kb::handlers::ThreadModel::Guarded> spi1{spi1_tag};
kb::handlers::Guarded<Key, decltype(spi1)> key{spi1, dev, timing, config};

static_cast<void>(spi1.Create());
static_cast<void>(key.Create());
if (key.EnsureStarted(kb::Milliseconds{500U}).IsOk()) {
  static_cast<void>(kb::handlers::isf15acp4::Clear(key, isf15acp4::kBlue, kb::Milliseconds{50U}));
  const auto s = key.Sample(kb::Milliseconds{10U});  // every 10 ms from the UI task
}
```

Tests: `tests/integration/isf15acp4/` (the driver's controller model and rig behind a `kb::iface::SpiDevice` and kb
pins, the model's clock on the RTOS clock, both thread models).
