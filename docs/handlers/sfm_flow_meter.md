# sfm_flow_meter handler

SFM3000 to SFM6000D I2C mass flow meters, through `kb::handlers::sfm_flow_meter::Handler<Bus>`
(`kb/handlers/bindings/sfm_flow_meter.hpp`, driver `driver-sfm-flow-meter` at the pin in
`components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `sample` (flow in ml/min, second flow, temperature, status), `state` (the driver's mode), `fresh` (read by this sample) |
| Command | `StartMeasurement(io, gas, timeout, o2_permille)` | Start a gas or mixture and wait for the first sample |
| Command | `StopMeasurement(io, timeout)` | Stop the measurement |
| Command | `SetAveraging(io, n, timeout)` | Averaging while idle |
| Command | `SetSleep(io, asleep, timeout)` | Sleep mode, or wake up |
| Command | `ReadSample(io, timeout)` | One sample now, outside the snapshot |
| Command | `ReadTemperature(io, timeout)` | The temperature by a pointer sequence (SF05 parts) |
| Command | `Command{Op::...}` with `Request()` | Oxygen fraction of a running mixture, soft reset, hard reset |

`io` is either facade; the calls are the same on an owner bus and on a guarded bus. A sample with nothing new from
the sensor (`NoData`) is not a failure: the snapshot keeps the last sample with `fresh = false`. A sensor that is not
measuring gives a snapshot with its `state` and no I/O.

## Thread model

`kRequiresOwner = false`, owner by preference. A flow meter is read at its sample rate (0.5 ms to 64 ms periods)
by one loop: on an owner bus the owner task samples every `sample_period` and other tasks read `Latest()` without a
lock. Every wait of the start-up is polled through the driver's `Poll()`, so the owner never blocks on a start. The
commands use the driver's own bounded wait (`AwaitPending()`, a few milliseconds); a guarded bus works too.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Boot + Identify` | polled | `BeginInitialize()`, then `Poll()` until done: wake, product identifier, averaging | `Config::identify_deadline` |
| 1 | `Configure` (optional) | polled | `StartMeasurement(gas, o2_permille)`, then `Poll()` until the first sample | `Config::start_deadline` |

Stage 1 runs when `Config::measure_on_start` is true. The /RES pin (SFM4200) is the driver's own `HardReset()`
hook, so there is no `HostReset` stage. On a failure the report names the stage with the driver's error in `detail`;
the next `EnsureStarted()` starts again from stage 0.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `DeviceReset`, `StateUnknown`, `NotInitialized` | `NotReady` | Faulted at once (the measurement ended by itself, or the mode is lost) |
| `Stale` | `NotReady` | Counts toward `fault_after` |
| `NoData` | `Empty` | In the snapshot: not a failure; from `ReadSample()`: counts |
| `Bus` | `BusError` | Counts toward `fault_after` |
| `NoDevice` | `NoDevice` | Counts toward `fault_after`; at start-up the start fails |
| `Crc` | `CrcMismatch` | Counts toward `fault_after` |
| `DeviceIdMismatch`, `DeviceFault`, `StatusMismatch`, `InvalidReply` | `Fault` | Counts toward `fault_after` |
| `InvalidArgument`, `Timeout`, `Busy`, `Unsupported`, `OutOfRange` | the same name | `InvalidArgument` is refused (recorded, not counted); the others count toward `fault_after` |

Recovery is the generic plan: `Stop()` when measuring and `Deinitialize()`, then the start-up again, which identifies
the sensor and starts the configured gas. A soft or hard reset command does the same inside the command.

## Example

```cpp
using Bus = board::I2c1;                                          // a kb::iface::I2cBus
using Flow = kb::handlers::sfm_flow_meter::Handler<Bus>;
kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Owner> i2c1{bus};
kb::handlers::OwnerAccess<Flow, decltype(i2c1)> flow{i2c1, bus, timing, config};

static_cast<void>(flow.Create());
flow.RequestStart();                                              // the owner task polls the plan
const auto s = flow.Latest();
if (s.IsOk() && s.Value().value.fresh) {
  const int32_t ml_per_min = s.Value().value.sample.flow_ml_per_min;
}
```

Tests: `tests/integration/sfm_flow_meter/` (the driver's chip model behind a `kb::iface::I2cBus`, its clock on the
RTOS clock, both thread models).
