# fdo2 handler

FDO2 and FDO2-G2 optical oxygen sensor modules on a UART, through `kb::handlers::fdo2::Handler<Uart>`
(`kb/handlers/bindings/fdo2.hpp`, driver `driver-fdo2` at the pin in `components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `oxygen`: partial pressure (10^-3 hPa), temperature, status word, quality, warm-up flag (`#MOXY`) |
| Command | `MeasureOxygen(io, timeout)` | One oxygen reading now |
| Command | `MeasureRaw(io, timeout)` | One raw reading: phase, signals, ambient light, pressure, humidity |
| Command | `Identify(io, timeout)` | Flash the module's LED |
| Command | `Resync(io, timeout)` | Find the module again after `StateUnknown` |
| Command | `PowerCycle(io, timeout)` | Supply off, wait, on, start again (the timeout must cover the power-up) |
| Command | `CalibrateAir(io, steady_s, timeout, ppm)` | High point in air (a flash write: needs `allow_persistent_writes`) |
| Command | `Command{Op::...}` with `Request()` | Zero and high-point calibration, broadcast on and off, baud rate |

`io` is either facade; the calls are the same on an owner bus and on a guarded bus.

## Thread model

`kRequiresOwner = false`, owner by preference. A UART is point to point: its owner task serves this module alone, so
a reading that waits for its reply (up to `measurement_ms`, 150 to 300 ms) blocks nobody else, and other tasks read
`Latest()` without a lock. The start-up is polled through the driver's `Poll()` (the power-up wait is about a
second), so the owner never blocks on a start. A guarded port works for sporadic readings; its callers then hold the
locks for one reading.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Power` | sync | `BeginInitialize()`: configuration check, UART up, the supply on through the driver's pin hook | `Config::power_deadline` |
| 1 | `Boot + Link + Identify` | polled | `Poll()` until initialised: the `power_up_ms` wait, optional stream check, `#VERS`, `#IDNR` | `Config::boot_deadline` |

The supply switch is the driver's own hook (`GpioSet(Power)`): without `Config::power` the board powers the module
and the hook reports success, so `PowerCycle()` then restarts without cutting the supply. On a failure the report names
the stage with the driver's error in `detail`; the next `EnsureStarted()` starts again from stage 0.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `DeviceFault`, `StateUnknown`, `NotInitialized` | `Fault`, `NotReady`, `NotReady` | Faulted at once (a potentially fatal module error, an unconfirmed flash write or baud change) |
| `NotReady` | `NotReady` | Counts toward `fault_after` (warming up) |
| `Bus` | `BusError` | Counts toward `fault_after`; at start-up the start fails |
| `Timeout` | `Timeout` | Counts toward `fault_after` |
| `Crc` | `CrcMismatch` | Counts toward `fault_after` |
| `DeviceIdMismatch`, `DeviceError`, `InvalidReply`, `EchoMismatch`, `SensorFault` | `Fault` | Counts toward `fault_after` |
| `OutOfRange`, `CalibrationRejected` | `OutOfRange` | Counts toward `fault_after` |
| `InvalidArgument`, `NotPermitted` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Busy`, `BroadcastActive` | `Busy` | Counts toward `fault_after` |
| `Unsupported` | `Unsupported` | Counts toward `fault_after` |

Recovery is the generic plan: `Deinitialize()` (the supply off, never during a flash write), then the start-up
again with its power-up wait.

## Example

```cpp
using Uart = board::Uart2;                                        // a kb::iface::UartPort
using O2 = kb::handlers::fdo2::Handler<Uart>;
kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Owner> uart2{uart};
kb::handlers::OwnerAccess<O2, decltype(uart2)> o2{uart2, uart, timing, config};   // config.handler.sample_period

static_cast<void>(o2.Create());
o2.RequestStart();                                                // the owner task polls the power-up
const auto s = o2.Latest();
if (s.IsOk()) {
  const int32_t mhpa = s.Value().value.oxygen.o2_mhpa;
}
```

Tests: `tests/integration/fdo2/` (the driver's byte-level module model behind a `kb::iface::UartPort`, its time on
the RTOS clock, the supply switch as a kb pin, both thread models).
