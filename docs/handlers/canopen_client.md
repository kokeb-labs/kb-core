# canopen_client handler

A CANopen client on one CAN bus (NMT master, SDO client, heartbeat and EMCY consumer, SYNC producer, PDO and CiA 402
helpers), through `kb::handlers::canopen_client::Handler<Can>` (`kb/handlers/bindings/canopen_client.hpp`, driver
`driver-canopen-client` at the pin in `components/drivers/drivers.lock.yml`).

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `nodes[]` (state, alive, boot-up seen, heartbeat count of each watched node), `tpdos[]` (latest frame of each subscribed TPDO), `last_emcy` and `emcy_count`, receive counters, `last_abort` |
| Command | `SendNmt(io, command, node, timeout)` | An NMT command to one node or every node (0) |
| Command | `SdoRead(io, object, timeout)` | An object of 1, 2 or 4 bytes, by expedited upload |
| Command | `SdoWrite(io, object, value, timeout)` | An object of 1, 2 or 4 bytes, confirmed |
| Command | `GuardNode(io, node, timeout)` | One node-guarding exchange: the state the node reports |
| Command | `ReadDriveState(io, node, timeout)` | The CiA 402 state of a drive |
| Command | `DriveEnable`, `DriveDisable`, `DriveQuickStop` `(io, node, timeout)` | The CiA 402 state machine walks; disable is the switch-off |
| Command | `Command{Op::...}` with `Request()` | Fault reset, mode of operation, SYNC, RPDO, clear boot-up |

## Thread model

`kRequiresOwner = true`, owner bus only (`BusDomain<Can, ThreadModel::Owner>` with `OwnerAccess<Handler>`; a guarded
facade does not compile). Heartbeats, EMCY messages and TPDOs arrive without being asked for: the receive path must be
drained continuously or the controller's queue overflows and the heartbeat consumer judges live nodes dead. Each
service of the owner task routes up to `Config::frames_per_service` received frames (`Poll(0)`), produces the SYNC
when one is due, takes the queued EMCY messages and publishes the snapshot. A command runs on the owner task and
blocks it for as long as the driver waits (an SDO reply, a drive state change), each wait bounded by the driver
configuration.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Link` | sync | `EnsureInitialized()`: configuration checked, controller started | `Config::link_deadline` |
| 1 | `Configure` | sync, optional | `MonitorNode()` per watched node, `SubscribeTpdo()` per TPDO, `ConfigureSync()`, and `SendNmt(Start, 0)` when `start_nodes` | `Config::configure_deadline` |

Stage 1 is skipped when no node, TPDO, SYNC period or NMT start is configured. The client has no pin of its own, so
there is no `Power` or `HostReset` stage.

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `NotInitialized` | `NotReady` | Faulted at once |
| `NodeState` | `NotReady` | Counts toward `fault_after` (the node is in a state that ignores the request) |
| `Bus` | `BusError` | Counts toward `fault_after` (a send the controller refused, a receive error) |
| `Timeout` | `Timeout` | Counts toward `fault_after` |
| `SdoAbort`, `Protocol`, `InvalidFrame`, `DriveFault`, `DeviceFault`, `DeviceIdMismatch` | `Fault` | Counts toward `fault_after`; an abort's code is in `Snapshot::last_abort` |
| `BufferTooSmall` | `Overflow` | Counts toward `fault_after` |
| `InvalidArgument`, `Crc`, `Busy`, `Unsupported` | the same name (`CrcMismatch` for `Crc`) | `InvalidArgument` is refused (recorded, not counted); the others count toward `fault_after` |

The periodic sample records a success whenever the receive path works, so failures of commands to a single node
fault the handler only when no successful sample comes between them; a controller that keeps failing to receive
faults it at the threshold. Recovery is the generic plan: `Deinitialize()` (no bus traffic), then the start-up again.

## Example

```cpp
using Can = board::Can0;                                          // a kb::iface::CanBus
using Client = kb::handlers::canopen_client::Handler<Can>;
kb::handlers::BusDomain<Can, kb::handlers::ThreadModel::Owner> can0{can};
kb::handlers::OwnerAccess<Client, decltype(can0)> co{can0, can, timing, config};

static_cast<void>(co.Create());
// owner task, every step: static_cast<void>(can0.Service(kb::rtos::Now(), co));
if (co.EnsureStarted(kb::Milliseconds{100U}).IsOk()) {
  const kb::Result<uint32_t> type =
      kb::handlers::canopen_client::SdoRead(co, {.index = 0x1000U, .node = 5U, .sub = 0U, .size = 4U}, kb::Milliseconds{300U});
  static_cast<void>(kb::handlers::canopen_client::DriveEnable(co, 5U, kb::Milliseconds{3000U}));
}
```

Tests: `tests/integration/canopen_client/` (the driver's simulated bus with two nodes behind a `kb::iface::CanBus`,
owner model).
