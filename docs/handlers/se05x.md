# se05x handler

SE050, SE051 and SE052 secure elements (T=1 over I2C, the IoT applet, platform SCP03), through
`kb::handlers::se05x::Handler<Bus, Crypto, WithReset>` (`kb/handlers/bindings/se05x.hpp`, driver `driver-se05x`).
The driver is a submodule at `components/drivers/driver-se05x`, pinned to v0.1.0-rc.1 in
`components/drivers/drivers.lock.yml`; the binding is built with `KB_ENABLE_SE05X=ON`, and in the integration builds.

| Parameter | What the HAL gives |
| --- | --- |
| `Bus` | A `kb::iface::I2cBus` (or `I2cBusRef`) with the chip at `Config::chip.i2c_address` (0x48) |
| `Crypto` | The driver's crypto port (`::se05x::CryptoPort`): key handles, a hardware key store or a library context behind them. kb-core defines no crypto contract; `::se05x::ReferenceCrypto` is for host tests only |
| `WithReset` | `true` when the board wires the ENA line (SE050, SE051) or RST_N (SE052) and gives it in `Config::reset` |
| `HookTiming` | The clock and the short delay of the board |
| `Config` | The driver's `::se05x::Config`, the handler settings, platform SCP03 key handles, the lifecycle expectation, the run line, the stage deadlines |

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Command | `ReadStatus(io, timeout)` | `DeviceStatus` from the driver's memory, no I/O: state, family, applet version, OEF id, lifecycle (transport lock, platform SCP03 required and open), whether answers are authenticated and traffic confidential, the APDU count, the open count, the last status word |
| Command | `CheckAlive(io, timeout)` | One GetVersion round trip; fails with `DeviceIdMismatch` (fatal) when another applet answers |
| Command | `GetRandom(io, out, timeout)` | 1 to 256 bytes from the chip's RNG |
| Command | `ReadUniqueId(io, timeout)` | The 18-byte unique id, read in the clear (trust it through `ReadAttested`) |
| Command | `ReadObject(io, id, offset, out, timeout, session)` | Exactly `out.size()` bytes (1 to 256) of a binary object from `offset` |
| Command | `ReadObjectAll(io, id, out, timeout, session)` | A whole object of any size (a certificate): its size, then 256-byte chunks |
| Command | `ReadAttested(io, id, attester, out, timeout, session)` | A whole object of up to 256 bytes (a public key, a small file) with an attestation, returned only when the signature, the fresh host nonce, the timestamp order and the unique id verify against `attester.trusted_public` |
| Command | `Sign(io, with, digest, signature, timeout, session)` | ECDSA over a digest with the key object `with.key`; the DER signature |
| Command | `Verify(io, with, digest, signature, timeout, session)` | ECDSA verification; `true` or `false`, both a successful call |
| Command | `Command{Op::...}` with `Request()` | `ReadSize`, `ObjectExists`, and every call above with its raw fields |

`io` is either facade (`Guarded<H>` or `OwnerAccess<H>`); the calls are the same on both. `Command` and `Reply` are
fixed-size, trivially copyable values (each at most 384 bytes, checked at compile time): data is copied into the
command and out of the reply in the caller's task, so no pointer to caller memory crosses a task. There is no periodic read
(`Sample()` returns `Unsupported`): nothing talks to the chip behind the application's back, which matters for the
chip's APDU budget (below).

Applet sessions are per call. `session` defaults to the default session; `AesKeySession(credential, key)` names an
AESKey authentication object on the chip and the crypto port's handle of its AES key. The handler opens the
session, runs the one command and closes the session inside the same call, so the whole transaction runs while the
bus is held, and the next call starts from the default session again. The same handle serves every call (the
driver derives session keys from it and releases only those). UserID and ECKey sessions are not offered: a UserID
value is a secret and would sit in the command queue, and an ECKey session needs a verified ECKA key read in the
same open of the device.

## Thread model

`kRequiresOwner = false`. A secure element is the typical shared device: several tasks need a signature, a random
number or a certificate now and then, and none of them should own the bus. A guarded bus
(`BusDomain<Bus, ThreadModel::Guarded>` with `Guarded<Handler>`) is the natural choice: each call takes the
handler's lock, then the bus lock, both bounded by the caller's timeout, runs the whole driver call (with its
session, when there is one) and releases both. The chip sees one complete T=1 transaction per call.

An owner bus works too; the owner task then runs each command to its end inside `Service()`, and a command can
take the driver's command timeout (`chip.timeouts`: 2 s for most commands, 5 s for attested reads, 20 s for key
generation). Do not put a real-time device on the same owner bus. In both models the timeout of a call must cover
the chip's own time for that command: on an owner bus a request that times out before the owner takes it is not
sent, and one that is already running finishes with its reply dropped.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline | Optional |
| --- | --- | --- | --- | --- | --- |
| 0 | `Link + Identify + Secure` | sync | `SetPlatformKeys(config.platform)` when the HAL gives key handles (no I/O), then `Open()`: the configuration checked, the T=1 link opened (boot wait, ATR or CIP), the card identification data read, the IoT applet selected, its feature bits and lifecycle state read, the capability probes run, platform SCP03 opened when `chip.scp_policy` or the chip requires it. Skipped when the device `IsOpen()` | `Config::open_deadline` | no |
| 1 | `Verify` | sync | No I/O: the capabilities `Open()` read against `Config::expect` | `Config::verify_deadline` | yes: `expect.enabled` |

Every wait of `Open()` is bounded by the link (`chip.boot_wait_ms`, `chip.atr_poll_budget_ms`, the command
timeouts); the stage deadline bounds the whole start in `EnsureStarted()`. On a failure the handler closes the
device (`Close()`), destroys the driver and stays not started; the report names the stage and carries the driver's
error in `detail`, and the next `EnsureStarted()` runs the plan again from stage 0.

The platform SCP03 keys are handles of the crypto port, never key bytes; the HAL creates them before the handler
starts, and `KeysGiven()` (a valid ENC or MAC handle) turns the call on. ADR-0010 lists them as a `Secure` stage
of their own before `Link`; a plan must list its stages in kind order, so they are the first step of stage 0.

`Config::expect` checks what the product expects of its chip after the open:

| Field | Refused with |
| --- | --- |
| `family` (`Unknown` accepts any) | `DeviceIdMismatch` |
| `min_applet` (0.0.0 accepts any) | `DeviceIdMismatch` |
| `applet_locked` (`Expect::Any`, `No`, `Yes`) | `WrongState` |
| `platform_scp_required` (`Expect::Any`, `No`, `Yes`) | `WrongState` |
| `authenticated_answers` (platform SCP03 open with R-MAC) | `WrongState` |

There is no `Power` or `HostReset` stage: the driver drives the run line itself (it raises ENA when the link opens
and power-cycles through it in its recovery ladder).

## Health

The driver's state after each call decides first: a call that leaves the driver `NeedsReopen` (a reset, a failed
secure-channel check), `Dead` (the transport ladder is exhausted) or `Closed` faults the handler at once, whatever
its error. The driver errors below fault at once too; every other failure counts toward
`handler.health.fault_after`, and a success resets the count.

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `NotInitialized`, `NeedsReselect` | `NotReady` | Faulted at once (the device or the applet selection is lost) |
| `WrongState` | `NotReady` | Counts; at start-up: platform SCP03 required without keys, or a `Verify` mismatch |
| `TransportDead` | `Fault` | Faulted at once (the link's recovery ladder is exhausted) |
| `BusStuck` | `BusError` | Faulted at once (SCL or SDA held low: only a power cycle clears it). With `WithReset` the driver's ladder power-cycles through the run line inside the call, and the call returns `NeedsReselect` (fatal) |
| `DeviceIdMismatch`, `Counterfeit` | `Fault` | Faulted at once (not the chip that was opened, or no IoT applet) |
| `Bus` | `BusError` | Counts |
| `Nack` | `NoDevice` | Counts; at start-up: no chip answers |
| `Timeout` | `Timeout` | Counts (a plain timeout keeps the device usable: the link resynchronises) |
| `Crc` | `CrcMismatch` | Counts |
| `Busy` | `Busy` | Counts |
| `ThroughputLimited` | `Busy` | Counts; the driver has reset the chip once (SW 0x66A6: the APDU budget) |
| `InvalidArgument`, `ObjectNotFound`, `ObjectExists`, `PolicyInvalid`, `IrreversibleNotConfirmed` | `InvalidArgument` | Refused: recorded as the last error, not counted against the chip (a caller's mistake) |
| `Unsupported`, `PolicyDenied`, `NotImplemented` | `Unsupported` | Counts |
| `BufferTooSmall` | `Overflow` | Counts |
| `MemoryFull` | `Full` | Counts |
| `DeviceFault`, `FrameError`, `Resynced`, `AuthFailed`, `AuthLocked`, `ConditionsNotSatisfied`, `SecureChannelError`, `AttestationInvalid`, `ChipSwUnknown`, `CryptoFailure` | `Fault` | Counts, unless the driver's state is lost (a `SecureChannelError` under platform SCP03 leaves it `NeedsReopen`: fatal) |

The code in `Detail()` and in the health record is the driver's `::se05x::Error`; the chip's status word of the
last response is in `ReadStatus().last_sw`. The application's own mistakes that map to `InvalidArgument` (an
object that does not exist, an invalid policy) are refused: recorded as the last error, never counted against the
chip. A policy refusal (`PolicyDenied`, `Unsupported`) still counts toward `fault_after`.

`RequestRecovery()` runs the se05x plan: the driver's `Recover()` (drop every session without sending, select the
applet again, open platform SCP03 again, and climb the link's ladder once when the select fails on the transport),
then `Close()`, then, with `WithReset` and when the ladder failed, a power cycle of the run line, then the whole
start-up plan again. Sessions and transient objects do not survive a recovery. Backoff and the attempt limit
come from `handler.recovery`.

## Security notes

- Keys are handles. The platform SCP03 keys and the AESKey session key are `KeyRef` handles of the HAL's crypto
  port; no key byte is in `Config`, `Command`, `Reply`, the health record or the start-up report.
- Nothing secret is logged or published: `DeviceStatus` holds the state, versions, counters and the last status
  word only, and the handler has no snapshot.
- The request queue of an owner bus keeps the last command and reply in its slots (digests, signatures, object
  data, random bytes). None is a secret in this API; do not use `GetRandom()` output as long-term key material
  (generate keys inside the chip).
- Over a plain bus the chip's answers can be forged by whoever can drive the wires. Use platform SCP03
  (`chip.scp_policy = Require`, key handles in `Config::platform`) and `expect.authenticated_answers = true` when
  the bus can be probed; `ReadStatus()` tells whether answers are authenticated and traffic is confidential.
- Trust identity through attestation: `ReadAttested()` returns data only after the driver has verified the
  signature, the fresh nonce it drew from the host's crypto port, the timestamp order and the unique id against a
  public key the application got from a trusted source (a verified certificate).
- The chip limits APDUs (SE052F, possibly SE051: SW 0x66A6 after its budget, `ThroughputLimited`). The handler
  sends nothing on its own, and `ReadStatus().apdu_count` shows what was sent; keep periodic chip traffic small.
- Irreversible operations (lock, factory reset, platform SCP03 requirement) are not reachable through the handler.
- A handler is not a safety function: a safety layer must reach its safe state without a call to the secure element
  succeeding.

## Example

```cpp
using Bus = board::I2c1;              // a kb::iface::I2cBus
using Crypto = board::KeyStore;       // the board's ::se05x::CryptoPort (hardware key store)
using Se = kb::handlers::se05x::Handler<Bus, Crypto, true>;

kb::handlers::se05x::Config config{};
config.chip.scp_policy = ::se05x::ScpPolicy::Require;   // platform SCP03 always
config.platform.keys.enc = board::kPlatformEncHandle;   // handles of the key store, never keys
config.platform.keys.mac = board::kPlatformMacHandle;
config.expect.enabled = true;
config.expect.family = ::se05x::Family::Se052F;
config.expect.authenticated_answers = true;
config.reset.pin.pin = kb::iface::GpioOutputRef{board::se_reset};
config.reset.is_enable_pin = false;                     // SE052: RST_N

kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c1{bus};
kb::handlers::Guarded<Se, decltype(i2c1)> se{i2c1, bus, key_store, timing, config};
static_cast<void>(i2c1.Create());
static_cast<void>(se.Create());

if (se.EnsureStarted(kb::Milliseconds{100U}).IsOk()) {
  namespace hs = kb::handlers::se05x;
  const hs::SignWith device_key{::se05x::ObjectId{0x00001001U}, ::se05x::EcSignAlgo::Sha256};
  std::array<uint8_t, hs::kMaxSignature> signature{};
  const kb::Result<std::size_t> n = hs::Sign(se, device_key, digest, signature, kb::Milliseconds{3000U});

  std::array<uint8_t, 1024> certificate{};
  const kb::Result<std::size_t> size =
      hs::ReadObjectAll(se, ::se05x::ObjectId{0x00004001U}, certificate, kb::Milliseconds{3000U});
}
if (se.Health().Value().state == kb::handlers::HealthState::Faulted) {
  static_cast<void>(se.RequestRecovery(kb::Milliseconds{5000U}));
}
```

Tests: `tests/integration/se05x/` (12 cases, built with `KB_ENABLE_SE05X=ON`): the driver's own chip model chain
(`tests/model`: the T=1 target, the SCP target and the IoT applet model) behind a `kb::iface::I2cBus`, the driver's
`ReferenceCrypto` as the crypto port, both thread models; start-up success and failures (no applet, no chip,
platform SCP03 without keys, a `Verify` mismatch), every app call, an AESKey session per call, health thresholds,
fatal errors and recovery with and without the run line.
