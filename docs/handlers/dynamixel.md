# dynamixel handler

DYNAMIXEL smart servos (protocol 1.0 or 2.0) on one half-duplex UART bus, several IDs per bus, through
`kb::handlers::dynamixel::Handler<Uart>` (`kb/handlers/bindings/dynamixel.hpp`, driver `driver-dynamixel` at the pin
in `components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `feedback[]` (position, velocity, effort, voltage, temperature) of every listed servo, `count`, `alert_mask` (bit n: servo n reported a hardware alert) |
| Command | `SetTorque(io, servo, enable, timeout)` | Torque of one servo on or off |
| Command | `SetGoalPosition(io, servo, counts, timeout)` | Goal position in counts |
| Command | `ReadFeedback(io, servo, timeout)` | The feedback block of one servo, read now |
| Command | `WriteRegister(io, servo, reg, value, timeout)`, `ReadRegister(io, servo, reg, timeout)` | A typed register of the servo's family |
| Command | `Ping(io, id, timeout)` | Model, firmware and family of one ID |
| Command | `DisableTorqueAll(io, family, timeout)` | Torque off for every servo of a family, one broadcast: the bus stop |
| Command | `Command{Op::...}` with `Request()` | Reboot, resync (wait for a quiet line) |

A servo is named by its `dynamixel::ServoRef` (ID and control-table family). `io` is either facade.

## Thread model

`kRequiresOwner = false`: the servos only answer when asked, so occasional guarded calls are correct. The owner model
is the recommended one for a servo loop: the owner task reads the feedback of every listed servo every
`sample_period` and serves goal positions from the request queue, so a control task reads `Latest()` without touching
the bus. One handler serves one bus; every servo on it goes through that handler.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Link` | sync | `EnsureInitialized()`: configuration checked, port up, baud rate applied | `Config::link_deadline` |
| 1 | `Identify` | polled, optional | `Identify(id)` of one listed servo per step; with `check_family` the family must be the listed one | `Config::identify_deadline` |

Stage 1 is skipped when `Config::servo_count` is 0. The servos have no reset or enable pin, so there is no `Power` or
`HostReset` stage. A servo that does not answer fails stage 1 with `Timeout`; a servo of another family fails it with
`Fault` and `DeviceIdMismatch` in `detail`.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `LineFault` | `BusError` | Faulted at once (the line carries only 0x00 or 0xFF: stuck or floating) |
| `NotInitialized` | `NotReady` | Faulted at once |
| `Bus`, `EchoMismatch` | `BusError` | Counts toward `fault_after` |
| `Timeout` | `Timeout` | Counts toward `fault_after` |
| `Crc` | `CrcMismatch` | Counts toward `fault_after` |
| `DeviceError`, `InvalidReply`, `DeviceIdMismatch`, `DeviceFault` | `Fault` | Counts toward `fault_after` |
| `InvalidArgument`, `NotPermitted` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Busy`, `Unsupported` | `Busy`, `Unsupported` | Counts toward `fault_after` |

A sample fails when any listed servo fails (its error is the sample's), so a servo that stops answering faults the
handler at the threshold. Recovery is the generic plan: `Deinitialize()` (no bus traffic, torque stays as it is),
then the start-up again.

The port's `DelayUs` hook goes through the hook timing (a spin below 1 ms, a sleep from 1 ms); the driver uses it
for the reboot and baud change settle times.

## Example

```cpp
using Uart = board::ServoBus;                                     // a kb::iface::UartPort
using Servos = kb::handlers::dynamixel::Handler<Uart>;
kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Owner> arm{uart};
kb::handlers::OwnerAccess<Servos, decltype(arm)> servos{arm, uart, timing, config};

static_cast<void>(servos.Create());
// owner task, every step: static_cast<void>(arm.Service(kb::rtos::Now(), servos));
constexpr dynamixel::ServoRef kShoulder{1U, dynamixel::Family::X430};
if (servos.EnsureStarted(kb::Milliseconds{500U}).IsOk()) {
  static_cast<void>(kb::handlers::dynamixel::SetTorque(servos, kShoulder, true, kb::Milliseconds{20U}));
  static_cast<void>(kb::handlers::dynamixel::SetGoalPosition(servos, kShoulder, 2048, kb::Milliseconds{20U}));
}
```

Tests: `tests/integration/dynamixel/` (the driver's servo bus model with three protocol 2.0 servos behind a
`kb::iface::UartPort`, both thread models).
