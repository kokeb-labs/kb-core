# bno08x handler

BNO080, BNO085 and BNO086 sensor hubs, SHTP over I2C, SPI or UART, through `kb::handlers::bno08x::Handler<Bus, kReset>`
(`kb/handlers/bindings/bno08x.hpp`, driver `driver-bno08x` at the pin in `components/drivers/drivers.lock.yml`). The
start-up
plan and the thread model follow ADR-0010 and ADR-0009.

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot::samples` | The latest sample per sensor (values in the report's Q point, accuracy, host time, count); configured sensors first |
| Snapshot | `Snapshot::ops` | The latest operation outcomes (`OpRecord`: operation number, error, detail, first values) |
| Snapshot | `product`, `restarts`, `dropped`, `hub_errors`, `state` | Product ID, hub restarts, dropped events, error reports, the session state |
| Read | `LatestSample(io, sensor)` | The newest sample of one sensor |
| Command | `SetSensor(io, sensor, feature, timeout)` | Set Feature (an interval of 0 turns the sensor off) |
| Command | `Tare(io, axes, basis, timeout)` | Tare now |
| Command | `ResetHub(io, timeout)` | Executable reset; the driver restores its sensors |
| Command | `ReadProductId(io, timeout)` | Product ID, into `Snapshot::product` |
| Command | `Command{Op::...}` with `Submit()` | Get feature, flush, persist tare, clear reorientation, sleep, on, save DCD, calibration enables |
| Wait | `WaitOp(io, op, timeout)` | Waits for an operation's record; its error when it failed |

Every command returns its operation number at once (`Reply::op`); the hub answers later and the outcome appears in
`Snapshot::ops`. `WaitOp()` polls `Latest()` and must not run on the owner task.

The bus type picks the transport: a `kb::iface::I2cBus` (SHTP over I2C), a `kb::iface::SpiDevice` (SHTP over SPI
with WAKE) or a `kb::iface::UartPort` (UART-SHTP). The pins come from `Config::pins` as functions with their polarity:
NRST asserted means in reset, WAKE asserted a wake request, H_INTN asserted that the hub has data (all three are
active low on the package). `kReset = true` (the default) gives the driver the NRST hook; on I2C a board without
H_INTN sets `chip.poll_without_int`.

## Thread model

Owner only: `kRequiresOwner = true`, and `Guarded<Handler>` does not compile. The hub streams reports and completes
every command asynchronously, so it is polled continuously: each `Service()` of the owner task runs `Measure()`,
which calls the driver's `Poll()` once and drains every pending event into the snapshot. Set `sample_period` to 0
(every pass) or at most a tenth of the fastest report period.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `HostReset` (`Link` with `kReset = false`) | sync | `Begin(chip)`: validate, bus up, NRST low (or join the running hub) | `Config::begin_deadline` |
| 1 | `Boot + Link` (`Identify` with `kReset = false`) | polled | `Poll()` until `Ready()`: the reset wait, the advertisement, reset complete and the initialise response; fails when the driver enters `Faulted` | `2 * chip.startup_timeout_ms` + the reset pulse + `Config::boot_margin` |
| 2 | `Configure` | polled | One `SetFeature()` per entry of `Config::sensors`, each polled until its `OpDone` | `chip.op_timeout_ms * (sensor_count + 1)` |

Without NRST the start joins a running hub, so the plan has no boot: ADR-0010's "Link without NRST" is stage 0, and
the wait for the hub's advertisement and initialise response is stage 1 (`Identify`), because a plan lists each kind
once and in order. A failure in stage 1 is `Timeout` (detail `Timeout`); in stage 2 it is the `OpDone` error or the
refusal of `SetFeature()` (for example `Unsupported` with `Variant` for a BNO086 sensor on a BNO085). The next
`EnsureStarted()` starts again from stage 0.

## Health

| Event or driver error | kb::Status | Effect |
| --- | --- | --- |
| The driver in `Faulted` (`NotInitialized`) | `NotReady` | Faulted at once; `RequestRecovery()` runs the plan again |
| A hub restart (`Reset` event, `HubReset`) | `NotReady` | One recorded failure: Degraded until the next good pass; the driver restores its sensors |
| `InvalidArgument` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Bus`, `Framing` | `BusError` | Counts toward `fault_after` |
| `Unsupported`, `NotAdvertised`, `Variant` | `Unsupported` | Counts toward `fault_after` |
| `Overflow` | `Overflow` | Counts toward `fault_after` |
| `Oversize` | `OutOfRange` | Counts toward `fault_after` |
| `DeviceFault`, `Refused` | `Fault` | Counts toward `fault_after` |
| `DeviceIdMismatch`, `Crc` | `NoDevice`, `CrcMismatch` | Never returned by this driver |
| `Timeout`, `Busy` | the same name | Counts toward `fault_after` |

Errors of an operation that completes later are in its `OpRecord`, not in the health record. On an owner bus that
samples every pass, a good pass follows each failure at once, so `fault_after` only counts failures that come with no
good sample between them.

## Example

```cpp
using Bus = board::I2c1;                                          // a kb::iface::I2cBus
using Imu = kb::handlers::bno08x::Handler<Bus>;
kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Owner> i2c1{bus};
kb::handlers::OwnerAccess<Imu, decltype(i2c1)> imu{i2c1, bus, timing, config};

static_cast<void>(imu.Create());
// The owner task: i2c1.Service(kb::rtos::Now(), imu) every millisecond.
if (imu.EnsureStarted(kb::Milliseconds{800U}).IsOk()) {
  const auto rv = kb::handlers::bno08x::LatestSample(imu, ::bno08x::SensorId::RotationVector);
  const auto tare = kb::handlers::bno08x::Tare(imu, ::bno08x::AxisMask::All,
                                               ::bno08x::TareBasis::RotationVector, kb::Milliseconds{5U});
  if (tare.IsOk()) {
    static_cast<void>(kb::handlers::bno08x::WaitOp(imu, tare.Value(), kb::Milliseconds{500U}));
  }
}
```

Tests: `tests/integration/bno08x/` (the driver's hub model behind a `kb::iface::I2cBus` and GPIO pins, owner model
only; that the guarded facade does not compile is a compile-fail test of its own).
