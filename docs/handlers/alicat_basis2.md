# alicat_basis2 handler

BASIS 2 mass flow meters and controllers on a serial line (ASCII protocol or Modbus RTU), through
`kb::handlers::alicat_basis2::Handler<Uart>` (`kb/handlers/bindings/alicat_basis2.hpp`, driver
`driver-alicat-basis2` at the pin in `components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `frame`: flow, setpoint, temperature, valve drive, total, gas and status flags of the last poll |
| Command | `ReadFrame(io, timeout)` | The data frame, polled now |
| Command | `SetSetpoint(io, flow, timeout)` | The setpoint in the instrument's flow unit (controllers) |
| Command | `ReadSetpoint(io, timeout)` | The current setpoint |
| Command | `CloseValve(io, timeout)` | The valve held closed: the switch-off, never refused for an unknown state |
| Command | `HoldValve(io, percent, timeout)`, `ResumeControl(io, timeout)` | Hold the valve, then closed-loop control again |
| Command | `Tare(io, duration_ms, timeout)`, `SetGas(io, gas, timeout)` | Tare at true zero flow; select the gas |
| Command | `Command{Op::...}` with `Request()` | Setpoint source, totalizer reset |

`io` is either facade; the calls are the same on an owner bus and on a guarded bus. The commands that can strand
the instrument (unit ID, Modbus address, baud rate, protocol switch, factory restore) need the driver's guard tokens
and are not in the command set.

## Thread model

`kRequiresOwner = false`. The instrument answers only when asked, and a transaction blocks for at most one reply
wait (`processing_ms` plus the line time, `slow_command_ms` for a command that writes non-volatile memory). A guarded
line (`BusDomain<Uart, ThreadModel::Guarded>` with `Guarded<Handler>`) suits the usual use: a setpoint now and then,
reads from several tasks. On an owner line the owner task polls the frame every `sample_period` and other tasks read
`Latest()`. One handler serves one instrument; it owns the driver's line machine (`Bus<Port>`).

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Link` | sync | `EnsureInitialized()`: port up, configuration checked, no instrument traffic | `Config::link_deadline` |
| 1 | `Identify` | sync | `Begin()`: poll, firmware, serial number, full scale, gas, controller settings; read only | `Config::identify_deadline` |

The instrument has no reset or enable pin the driver could use, so there is no `Power` or `HostReset` stage. On a
failure the report names the stage with the driver's error in `detail`; the next `EnsureStarted()` starts again at
stage 0.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `StateUnknown`, `NotInitialized` | `NotReady` | Faulted at once (a guarded command left the line settings in doubt) |
| `Bus` | `BusError` | Counts toward `fault_after` |
| `Timeout` | `Timeout` | Counts toward `fault_after`; at start-up the start fails |
| `Rejected`, `BadReply`, `DeviceIdMismatch`, `DeviceFault`, `VerifyMismatch` | `Fault` | Counts toward `fault_after` |
| `Busy`, `RateLimited` | `Busy` | Counts toward `fault_after` |
| `Unsupported`, `FirmwareTooOld` | `Unsupported` | Counts toward `fault_after` |
| `InvalidArgument`, `GuardNotConfirmed` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Crc`, `OutOfRange`, `Overflow` | `CrcMismatch`, `OutOfRange`, `Overflow` | Counts toward `fault_after` |

Recovery runs the driver's own `Recover()` first: it polls the configured ID and, with one instrument on the line,
scans for the one that answers and checks it is the same instrument. The ID, address or baud rate it adopted is kept
in the handler's configuration, then the plan runs again from stage 0.

## Example

```cpp
using Uart = board::Rs485;                                        // a kb::iface::UartPort
using Mfc = kb::handlers::alicat_basis2::Handler<Uart>;
kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Guarded> line{uart};
kb::handlers::Guarded<Mfc, decltype(line)> mfc{line, uart, timing, config};

static_cast<void>(line.Create());
static_cast<void>(mfc.Create());
if (mfc.EnsureStarted(kb::Milliseconds{3000U}).IsOk()) {
  static_cast<void>(kb::handlers::alicat_basis2::SetSetpoint(mfc, 12.5, kb::Milliseconds{500U}));
  const kb::Result<alicat_basis2::Frame> f = kb::handlers::alicat_basis2::ReadFrame(mfc, kb::Milliseconds{500U});
}
```

Tests: `tests/integration/alicat_basis2/` (the driver's instrument and wire model behind a `kb::iface::UartPort`,
the driver's clock on the model's virtual clock, both thread models).
