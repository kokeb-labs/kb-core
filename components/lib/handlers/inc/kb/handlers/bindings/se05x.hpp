// clang-format off: the table below keeps one row per line.
/**
 * @file se05x.hpp
 * @brief The se05x binding and handler: SE050, SE051 and SE052 secure elements (T=1 over I2C, IoT
 * applet, platform SCP03).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `BusPort<Bus>`: the driver's bus hooks over a kb::iface::I2cBus (I2cHooks), with NACK, timeout, bus error and stuck bus kept apart; `ClockPort`: 64-bit microseconds and delays from HookTiming; `ResetPort`: the enable or reset line (GpioHooks), with `WithReset`; the crypto port is the HAL's own (`Crypto`, a `::se05x::CryptoPort`) |
 * | Errors | `ToStatus()`: every `::se05x::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Link + Identify + Secure` = `SetPlatformKeys()` when the HAL gives key handles, then `Open()` (sync, every wait bounded by the link; skipped when `IsOpen()`); optional `Verify` = the lifecycle state against `Config::expect` |
 * | Thread model | Guarded (`kRequiresOwner = false`): a secure element used by several tasks, one held bus transaction per call; an owner bus works too |
 * | Health | A call that leaves the driver `NeedsReopen`, `Dead` or `Closed` faults at once, as do `NeedsReselect`, `TransportDead`, `BusStuck`, `DeviceIdMismatch`, `Counterfeit` and `NotInitialized`; other failures count to the threshold |
 * | Recovery | `Recover()` (the driver's ladder), `Close()`, a power cycle of the line when the ladder failed (with `WithReset`), then the plan again |
 * | App API | status (no I/O), alive check, random bytes, unique id, object reads (a chunk or a whole object), verified attested reads, ECDSA sign and verify by key id; an AESKey session per call when the object needs one |
 *
 * Applet sessions are not a start-up stage: a command that carries an AESKey `Session` opens it,
 * runs and closes it inside the one handler call, so the whole transaction runs under one held
 * bus (ADR-0010). Commands and replies are fixed-size values: no pointer to caller memory crosses
 * a task, and no secret is ever in them (key handles only, never key bytes).
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c1{bus};
 * kb::handlers::Guarded<kb::handlers::se05x::Handler<Bus, Crypto>, decltype(i2c1)> se{
 *     i2c1, bus, crypto, timing, cfg};
 * if (se.EnsureStarted(kb::Milliseconds{50U}).IsOk()) {
 *   std::array<uint8_t, 16> nonce{};
 *   static_cast<void>(kb::handlers::se05x::GetRandom(se, nonce, kb::Milliseconds{3000U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_SE05X_HPP
#define KB_HANDLERS_BINDINGS_SE05X_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "kb/handlers/handlers.hpp"
#include <se05x.hpp>

namespace kb::handlers::se05x {

/// @name Sizes of the fixed buffers in Command and Reply
/// @{
inline constexpr std::size_t kMaxData = 256U;      ///< Bytes one call reads or returns.
inline constexpr std::size_t kMaxDigest = 64U;     ///< Digest bytes of Sign and Verify (SHA-512).
inline constexpr std::size_t kMaxSignature = 144U; ///< DER ECDSA signature bytes (P-521 fits).
inline constexpr std::size_t kPointLength = 65U;   ///< Uncompressed P-256 public point.
inline constexpr std::size_t kUniqueIdLength = ::se05x::kUniqueIdLength; ///< The chip's unique id.
/// @}

/**
 * @brief The one map of the driver's errors to kb::Status (every enumerator, no default; each
 * group returns, which keeps the 40 enumerators inside one short function).
 */
[[nodiscard]] constexpr kb::Status ToStatus(::se05x::Error error) noexcept {
  switch (error) {
  case ::se05x::Error::Ok:
    return kb::Status::Ok;
  case ::se05x::Error::NotInitialized:
  case ::se05x::Error::WrongState:
  case ::se05x::Error::NeedsReselect:
    return kb::Status::NotReady;
  case ::se05x::Error::InvalidArgument:
  case ::se05x::Error::ObjectNotFound:
  case ::se05x::Error::ObjectExists:
  case ::se05x::Error::PolicyInvalid:
  case ::se05x::Error::IrreversibleNotConfirmed:
    return kb::Status::InvalidArgument;
  case ::se05x::Error::Bus:
  case ::se05x::Error::BusStuck:
    return kb::Status::BusError;
  case ::se05x::Error::Timeout:
    return kb::Status::Timeout;
  case ::se05x::Error::Crc:
    return kb::Status::CrcMismatch;
  case ::se05x::Error::Busy:
  case ::se05x::Error::ThroughputLimited:
    return kb::Status::Busy;
  case ::se05x::Error::Unsupported:
  case ::se05x::Error::PolicyDenied:
  case ::se05x::Error::NotImplemented:
    return kb::Status::Unsupported;
  case ::se05x::Error::BufferTooSmall:
    return kb::Status::Overflow;
  case ::se05x::Error::Nack:
    return kb::Status::NoDevice;
  case ::se05x::Error::MemoryFull:
    return kb::Status::Full;
  case ::se05x::Error::DeviceIdMismatch:
  case ::se05x::Error::DeviceFault:
  case ::se05x::Error::TransportDead:
  case ::se05x::Error::FrameError:
  case ::se05x::Error::Resynced:
  case ::se05x::Error::AuthFailed:
  case ::se05x::Error::AuthLocked:
  case ::se05x::Error::ConditionsNotSatisfied:
  case ::se05x::Error::SecureChannelError:
  case ::se05x::Error::AttestationInvalid:
  case ::se05x::Error::Counterfeit:
  case ::se05x::Error::ChipSwUnknown:
  case ::se05x::Error::CryptoFailure:
    return kb::Status::Fault;
  }
  return kb::Status::Fault; // not an enumerator
}

/**
 * @brief The driver errors after which the device cannot serve without a recovery: the device is
 * closed, the applet must be selected again, the transport is dead, the bus is stuck, or the chip
 * is not the one that was opened.
 */
inline constexpr std::array<::se05x::Error, 6> kFatalErrors{
    ::se05x::Error::NotInitialized, ::se05x::Error::NeedsReselect,    ::se05x::Error::TransportDead,
    ::se05x::Error::BusStuck,       ::se05x::Error::DeviceIdMismatch, ::se05x::Error::Counterfeit};

/** @brief True when `detail` (a driver error code) is one of kFatalErrors. */
[[nodiscard]] constexpr bool IsFatalDetail(uint32_t detail) noexcept {
  bool found = false;
  for (std::size_t i = 0U; i < kFatalErrors.size(); ++i) {
    found = found || (detail == DetailOf(kFatalErrors[i]));
  }
  return found;
}

/** @brief True when the device in `state` cannot serve a command (a recovery is needed). */
[[nodiscard]] constexpr bool IsLost(::se05x::DeviceState state) noexcept {
  return (state != ::se05x::DeviceState::Ready) && (state != ::se05x::DeviceState::InSession);
}

/**
 * @brief The driver's I2C bus port over a kb::iface::I2cBus. A failed transfer is reported by its
 * cause, because the link needs them apart: a NACK is the chip still busy (it polls), and only a
 * stuck bus needs a power cycle.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class BusPort : public ::se05x::BusInterface<BusPort<Bus>>, public I2cHooks<BusPort<Bus>, Bus> {
public:
  /** @brief Over `bus` with `timing`; no I/O. */
  BusPort(Bus& bus, const HookTiming& timing) noexcept : I2cHooks<BusPort<Bus>, Bus>(bus, timing) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transaction (never empty: the driver sends no address-only frame). */
  ::se05x::BusStatus Write(uint8_t addr7, ::se05x::ByteSpan data) noexcept {
    return this->BusWrite(addr7, data.data(), data.size()) ? ::se05x::BusStatus::Ok : cause();
  }
  /** @brief Hook: one read transaction of `out.size()` bytes. */
  ::se05x::BusRead Read(uint8_t addr7, ::se05x::MutableByteSpan out) noexcept {
    ::se05x::BusRead r{};
    if (this->BusRead(addr7, out.data(), out.size())) {
      r.status = ::se05x::BusStatus::Ok;
      r.count = out.size();
    } else {
      r.status = cause();
      r.count = 0U;
    }
    return r;
  }

private:
  // Why the last transfer failed, from the bus's failure reason, else from its kb::Status.
  [[nodiscard]] ::se05x::BusStatus cause() const noexcept {
    ::se05x::BusStatus s = ::se05x::BusStatus::BusError;
    switch (this->BusLastFailure()) {
    case kb::iface::I2cFailure::AddressNack:
    case kb::iface::I2cFailure::DataNack:
      s = ::se05x::BusStatus::Nack;
      break;
    case kb::iface::I2cFailure::Timeout:
      s = ::se05x::BusStatus::Timeout;
      break;
    case kb::iface::I2cFailure::BusStuck:
      s = ::se05x::BusStatus::BusStuck;
      break;
    case kb::iface::I2cFailure::Bus:
      s = ::se05x::BusStatus::BusError;
      break;
    case kb::iface::I2cFailure::None:
      s = FromStatus(this->Counters().last_status);
      break;
    }
    return s;
  }
  [[nodiscard]] static constexpr ::se05x::BusStatus FromStatus(kb::Status status) noexcept {
    ::se05x::BusStatus s = ::se05x::BusStatus::BusError;
    if ((status == kb::Status::NoDevice) || (status == kb::Status::Nack)) {
      s = ::se05x::BusStatus::Nack;
    } else if (status == kb::Status::Timeout) {
      s = ::se05x::BusStatus::Timeout;
    } else {
      s = ::se05x::BusStatus::BusError;
    }
    return s;
  }
};

/** @brief The driver's clock port: 64-bit microseconds (never wrap) and at-least delays. */
class ClockPort : public ::se05x::ClockInterface<ClockPort> {
public:
  /** @brief Over `timing`; no I/O. */
  explicit ClockPort(const HookTiming& timing) noexcept : timing_(timing) {}

  /** @brief Hook: monotonic microseconds since boot. */
  uint64_t NowUs() noexcept {
    return timing_.Now().Us();
  }
  /** @brief Hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    timing_.DelayUs(us);
  }

private:
  HookTiming timing_;
};

/**
 * @brief The board's enable or reset line (board values from the HAL). The pin is the "run" line:
 * asserted means the chip runs (ENA high on SE050 and SE051, RST_N released on SE052), so
 * `active_low` stays false for both wirings.
 */
struct ResetLine {
  OutputPin pin{};               ///< The line; absent: Reset() and PowerCycle() fail.
  uint32_t reset_off_us{1000U};  ///< Time held off for Reset().
  uint32_t power_off_us{10000U}; ///< Time held off for PowerCycle().
  uint32_t settle_us{0U};        ///< Wait after release (the driver waits its boot time too).
  bool is_enable_pin{true};      ///< ENA (SetEnable() drives it); false for RST_N (SE052).
};

/** @brief The driver's reset port over the run line (GpioHooks, one output). */
class ResetPort : public ::se05x::ResetInterface<ResetPort>, public GpioHooks<ResetPort, 1U, 0U> {
public:
  /** @brief Over the HAL's `line` with `timing`; no I/O. */
  ResetPort(const ResetLine& line, const HookTiming& timing) noexcept
      : GpioHooks<ResetPort, 1U, 0U>(std::array<OutputPin, 1U>{line.pin},
                                     std::array<InputPin, 0U>{}),
        timing_(timing), line_(line) {}

  /** @brief Hook: a real reset of the chip (line off, hold, on, settle). */
  bool Reset() noexcept {
    return pulse(line_.reset_off_us);
  }
  /** @brief Hook: a power cycle (line off for the longer time, on, settle). */
  bool PowerCycle() noexcept {
    return pulse(line_.power_off_us);
  }
  /** @brief Hook: drive ENA; false on a RST_N wiring (the driver then uses the S-block). */
  bool SetEnable(bool on) noexcept {
    return line_.is_enable_pin && this->DriveOutput(kLine, on);
  }

private:
  static constexpr std::size_t kLine = 0U;
  [[nodiscard]] bool pulse(uint32_t off_us) noexcept {
    bool ok = this->DriveOutput(kLine, false);
    if (ok) {
      timing_.DelayUs(off_us);
      ok = this->DriveOutput(kLine, true);
    }
    if (ok && (line_.settle_us != 0U)) {
      timing_.DelayUs(line_.settle_us);
    }
    return ok;
  }
  HookTiming timing_;
  ResetLine line_;
};

/** @brief The driver's port set: the bus, the clock, the HAL's crypto port, and the reset line. */
template <kb::iface::I2cBus Bus, ::se05x::CryptoPort Crypto, bool WithReset>
using PortsOf =
    std::conditional_t<WithReset, ::se05x::Ports<BusPort<Bus>, ClockPort, Crypto, ResetPort>,
                       ::se05x::Ports<BusPort<Bus>, ClockPort, Crypto>>;

/** @brief The driver type a handler over `Bus` owns. */
template <kb::iface::I2cBus Bus, ::se05x::CryptoPort Crypto, bool WithReset = false>
using Driver = ::se05x::Device<PortsOf<Bus, Crypto, WithReset>>;

/** @brief A three-way expectation of the Verify stage. */
enum class Expect : uint8_t {
  Any = 0, ///< Not checked.
  No = 1,  ///< Must be false.
  Yes = 2, ///< Must be true.
};

/** @brief True when `actual` meets `expect`. */
[[nodiscard]] constexpr bool Meets(Expect expect, bool actual) noexcept {
  bool ok = true;
  switch (expect) {
  case Expect::Any:
    ok = true;
    break;
  case Expect::No:
    ok = !actual;
    break;
  case Expect::Yes:
    ok = actual;
    break;
  }
  return ok;
}

/** @brief What the Verify stage checks after Open() (board values: what this product expects). */
struct Expectation {
  bool enabled{false};                              ///< Runs the Verify stage.
  ::se05x::Family family{::se05x::Family::Unknown}; ///< The part; Unknown accepts any.
  ::se05x::AppletVersion min_applet{};              ///< The oldest applet accepted; 0.0.0 any.
  Expect applet_locked{Expect::Any};                ///< The transport lock state.
  Expect platform_scp_required{Expect::Any};        ///< The chip's platform SCP03 requirement.
  bool authenticated_answers{false}; ///< Requires answers under R-MAC (platform SCP03 open).
};

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::se05x::Config chip{};  ///< The driver's configuration (address, waits, policy).
  HandlerConfig handler{}; ///< Health, recovery, polling.
  ::se05x::scp03::ChannelParams platform{}; ///< Platform SCP03 key handles; none: not given.
  Expectation expect{};                     ///< The Verify stage.
  ResetLine reset{};                        ///< The run line (used with WithReset).
  kb::Milliseconds open_deadline{10000U};   ///< Bound of the Open stage.
  kb::Milliseconds verify_deadline{50U};    ///< Bound of the Verify stage (no I/O).
};

/** @brief True when `params` carries key handles (the HAL asked for platform SCP03 keys). */
[[nodiscard]] constexpr bool KeysGiven(const ::se05x::scp03::ChannelParams& params) noexcept {
  return params.keys.enc.IsValid() || params.keys.mac.IsValid();
}

/** @brief How a command authenticates to the applet. */
enum class SessionUse : uint8_t {
  Default = 0, ///< The default session (no applet session).
  AesKey = 1,  ///< An AESKey session (SCP03 with the applet), opened and closed inside the call.
};

/** @brief The applet session of one command: handles only, never key bytes. */
struct Session {
  ::se05x::ObjectId credential{};      ///< The AESKey authentication object on the chip.
  ::se05x::KeyRef key{};               ///< The crypto port's handle of that AES key.
  SessionUse use{SessionUse::Default}; ///< Which session.
  ::se05x::scp03::SecurityLevel level{::se05x::scp03::SecurityLevel::Full}; ///< Its level.
};

/** @brief An AESKey session over `credential` with the crypto port's key handle `key`. */
[[nodiscard]] constexpr Session AesKeySession(::se05x::ObjectId credential,
                                              ::se05x::KeyRef key) noexcept {
  Session s{};
  s.credential = credential;
  s.key = key;
  s.use = SessionUse::AesKey;
  return s;
}

/** @brief The key and algorithm of Sign() and Verify(). */
struct SignWith {
  ::se05x::ObjectId key{};                               ///< The key object on the chip.
  ::se05x::EcSignAlgo algo{::se05x::EcSignAlgo::Sha256}; ///< The digest the chip expects.
};

/** @brief Who signs an attested read, and the trusted public key that checks it. */
struct Attester {
  ::se05x::ObjectId key{};                            ///< The attestation key object on the chip.
  std::array<uint8_t, kPointLength> trusted_public{}; ///< Its public key, from a trusted source.
  ::se05x::AttestationAlgo algo{::se05x::AttestationAlgo::EcSha256}; ///< The signature algorithm.
};

/** @brief What a verified attested read proved. */
struct Attestation {
  ::se05x::Timestamp timestamp{};                   ///< The chip's timestamp of the answer.
  std::array<uint8_t, kUniqueIdLength> unique_id{}; ///< The chip that signed.
  uint32_t object_size{0U};                         ///< The object size the chip signed.
  uint16_t length{0U};                              ///< Valid data bytes.
};

/** @brief The device as the driver knows it (no I/O; no secret). */
struct DeviceStatus {
  ::se05x::AppletVersion applet{};                          ///< Applet version from Open().
  uint32_t apdu_count{0U};                                  ///< APDUs sent, for the chip's budget.
  uint32_t open_count{0U};                                  ///< Times the device was brought up.
  uint16_t oef_id{0U};                                      ///< Card identification OEF id.
  uint16_t last_sw{0U};                                     ///< Status word of the last response.
  ::se05x::DeviceState state{::se05x::DeviceState::Closed}; ///< The driver's state.
  ::se05x::Family family{::se05x::Family::Unknown};         ///< The part, derived at Open().
  bool applet_locked{false};                                ///< The transport lock is set.
  bool platform_scp_required{false};                        ///< The chip demands platform SCP03.
  bool platform_scp_active{false};                          ///< Platform SCP03 is open.
  bool answers_authenticated{false};                        ///< Answers carry an R-MAC.
  bool traffic_confidential{false};                         ///< Both directions are encrypted.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadStatus = 0,   ///< The driver's state and lifecycle (no I/O).
  CheckAlive = 1,   ///< GetVersion: the chip answers and is the applet opened.
  GetRandom = 2,    ///< `length` bytes from the chip's RNG.
  ReadObject = 3,   ///< `length` bytes of object `id` from `offset` (0: the whole object).
  ReadSize = 4,     ///< The size of object `id`.
  ObjectExists = 5, ///< Whether object `id` exists.
  ReadAttested = 6, ///< An attested read of `id` (as ReadObject), verified with `attester`.
  Sign = 7,         ///< ECDSA over `digest` with key `sign.key`.
  Verify = 8,       ///< ECDSA verification of `signature` over `digest`.
};

/** @brief One command (a fixed-size value; key handles only). */
struct Command {
  std::array<uint8_t, kMaxDigest> digest{};       ///< Sign, Verify.
  std::array<uint8_t, kMaxSignature> signature{}; ///< Verify.
  Attester attester{};                            ///< ReadAttested.
  Session session{};                              ///< The applet session of this call.
  SignWith sign{};                                ///< Sign, Verify.
  ::se05x::ObjectId id{};       ///< ReadObject, ReadSize, ObjectExists, ReadAttested.
  uint16_t offset{0U};          ///< ReadObject, ReadAttested.
  uint16_t length{0U};          ///< Bytes asked (GetRandom, ReadObject, ReadAttested).
  uint8_t digest_length{0U};    ///< Valid bytes of `digest`.
  uint8_t signature_length{0U}; ///< Valid bytes of `signature`.
  Op op{Op::ReadStatus};        ///< What to do.
};

/** @brief The reply of a command. */
struct Reply {
  std::array<uint8_t, kMaxData> data{}; ///< Random bytes, object bytes or a signature.
  Attestation attestation{};            ///< ReadAttested.
  DeviceStatus status{};                ///< ReadStatus, CheckAlive.
  uint32_t value{0U};                   ///< ReadSize.
  uint16_t length{0U};                  ///< Valid bytes of `data`.
  bool flag{false};                     ///< ObjectExists; Verify: the signature is valid.
};

/// Command and Reply are copied through the owner queue and the guarded call: keep them small.
inline constexpr std::size_t kMaxMessageBytes = 384U;
static_assert(std::is_trivially_copyable_v<Command> && (sizeof(Command) <= kMaxMessageBytes),
              "Command: a small trivially copyable value");
static_assert(std::is_trivially_copyable_v<Reply> && (sizeof(Reply) <= kMaxMessageBytes),
              "Reply: a small trivially copyable value");

/** @brief A failure the binding finds before calling the driver (the driver's code as detail). */
template <typename T>
[[nodiscard]] constexpr kb::Result<T> Refuse(::se05x::Error error) noexcept {
  return kb::Result<T>::Failure(ToStatus(error), DetailOf(error));
}

/**
 * @brief The se05x handler.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 * @tparam Crypto The HAL's crypto port (key handles; kb-core defines no crypto contract).
 * @tparam WithReset True when the board wires the enable or reset line (`Config::reset`).
 */
template <kb::iface::I2cBus Bus, ::se05x::CryptoPort Crypto, bool WithReset = false>
class Handler : public HandlerCore<Handler<Bus, Crypto, WithReset>, Driver<Bus, Crypto, WithReset>,
                                   NoSnapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus, Crypto, WithReset>, Driver<Bus, Crypto, WithReset>,
                           NoSnapshot, Command, Reply>;
  using Dev = Driver<Bus, Crypto, WithReset>;

public:
  /// The start-up plan (ADR-0010 se05x plan): the open, then the optional lifecycle check.
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{static_cast<uint16_t>(stage::kLink | stage::kIdentify | stage::kSecure), StageMode::Sync,
        false},
       {stage::kVerify, StageMode::Sync, true}}};
  static constexpr bool kRequiresOwner = false;   ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::I2c;   ///< On an I2C bus.
  static constexpr std::size_t kOpenStage = 0U;   ///< Index of the open stage.
  static constexpr std::size_t kVerifyStage = 1U; ///< Index of the Verify stage.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, Crypto& crypto, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), bus_(bus, timing), clock_(timing), reset_(config.reset, timing),
        ports_(makePorts(crypto)), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Dev>& slot) noexcept {
    slot.emplace(ports_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call, Dev& driver) noexcept {
    return (call.index == kVerifyStage) ? verify(driver) : open(driver);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == kVerifyStage) ? config_.verify_deadline : config_.open_deadline;
  }
  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept {
    return (index != kVerifyStage) || config_.expect.enabled;
  }
  void AbortStartup(Dev& driver) noexcept {
    static_cast<void>(driver.Close());
  }
  [[nodiscard]] kb::Result<void> StopDriver(Dev& driver) noexcept {
    return LiftStatus(driver.Close(), &ToStatus);
  }
  [[nodiscard]] kb::Result<void> RecoverDriver(Dev& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<void> ladder = LiftStatus(driver.Recover(), &ToStatus);
    static_cast<void>(driver.Close());
    if constexpr (WithReset) {
      if (!ladder.IsOk()) {
        static_cast<void>(reset_.PowerCycle());
      }
    }
    lost_ = false;
    return ladder;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return lost_ || IsFatalDetail(detail);
  }
  [[nodiscard]] kb::Result<Reply> Run(Dev& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    const bool session = (c.session.use == SessionUse::AesKey) && (c.op != Op::ReadStatus);
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    const kb::Result<void> opened =
        session ? openSession(driver, c.session) : kb::Result<void>::Success();
    if (opened.IsOk()) {
      result = dispatch(driver, c);
      if (session) {
        const kb::Result<void> closed = LiftStatus(driver.EndSession(), &ToStatus);
        if (result.IsOk() && (!closed.IsOk())) {
          result = Forward<Reply>(closed);
        }
      }
    } else {
      result = Forward<Reply>(opened);
    }
    lost_ = IsLost(driver.State());
    return result;
  }
  /// @}

  /** @brief The bus port's transfer counters (NACKs while the chip works count as failures). */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return bus_.Counters();
  }

private:
  using Set = PortsOf<Bus, Crypto, WithReset>;

  [[nodiscard]] Set makePorts(Crypto& crypto) noexcept {
    if constexpr (WithReset) {
      return Set(bus_, clock_, crypto, reset_);
    } else {
      return Set(bus_, clock_, crypto);
    }
  }

  // --- Start-up stages.
  [[nodiscard]] kb::Result<StartProgress> open(Dev& driver) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    if (!driver.IsOpen()) {
      if (KeysGiven(config_.platform)) {
        r = LiftStatus(driver.SetPlatformKeys(config_.platform), &ToStatus);
      }
      if (r.IsOk()) {
        r = LiftStatus(driver.Open(), &ToStatus);
      }
    }
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }

  [[nodiscard]] kb::Result<StartProgress> verify(const Dev& driver) const noexcept {
    const Expectation& e = config_.expect;
    const ::se05x::Capabilities& caps = driver.GetCapabilities();
    ::se05x::Error why = ::se05x::Error::Ok;
    if ((e.family != ::se05x::Family::Unknown) && (caps.family != e.family)) {
      why = ::se05x::Error::DeviceIdMismatch;
    } else if (!caps.applet.AtLeast(e.min_applet)) {
      why = ::se05x::Error::DeviceIdMismatch;
    } else if (!Meets(e.applet_locked, caps.applet_locked)) {
      why = ::se05x::Error::WrongState;
    } else if (!Meets(e.platform_scp_required, caps.platform_scp_required)) {
      why = ::se05x::Error::WrongState;
    } else if (e.authenticated_answers && (!driver.AnswersAuthenticated())) {
      why = ::se05x::Error::WrongState;
    } else {
      // Every expectation met.
    }
    return (why == ::se05x::Error::Ok) ? kb::Result<StartProgress>::Success(StartProgress::Done)
                                       : Refuse<StartProgress>(why);
  }

  // --- Commands.
  [[nodiscard]] static kb::Result<void> openSession(Dev& driver, const Session& s) noexcept {
    ::se05x::AesKeySessionParams p{};
    p.credential = s.credential;
    p.keys.enc = s.key;
    p.level = s.level;
    return LiftStatus(driver.BeginAesKeySession(p), &ToStatus);
  }

  [[nodiscard]] static kb::Result<Reply> dispatch(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    switch (c.op) {
    case Op::ReadStatus:
      result = status(driver);
      break;
    case Op::CheckAlive:
      result = checkAlive(driver);
      break;
    case Op::GetRandom:
      result = random(driver, c);
      break;
    case Op::ReadObject:
      result = readObject(driver, c);
      break;
    case Op::ReadSize:
      result = readSize(driver, c);
      break;
    case Op::ObjectExists:
      result = exists(driver, c);
      break;
    case Op::ReadAttested:
      result = readAttested(driver, c);
      break;
    case Op::Sign:
      result = sign(driver, c);
      break;
    case Op::Verify:
      result = verifySignature(driver, c);
      break;
    }
    return result;
  }

  [[nodiscard]] static DeviceStatus statusOf(const Dev& driver) noexcept {
    const ::se05x::Capabilities& caps = driver.GetCapabilities();
    DeviceStatus s{};
    s.applet = caps.applet;
    s.apdu_count = driver.ApduCount();
    s.open_count = driver.OpenCount();
    s.oef_id = caps.oef_id;
    s.last_sw = driver.GetLastStatusWord();
    s.state = driver.State();
    s.family = caps.family;
    s.applet_locked = caps.applet_locked;
    s.platform_scp_required = caps.platform_scp_required;
    s.platform_scp_active = caps.platform_scp_active;
    s.answers_authenticated = driver.AnswersAuthenticated();
    s.traffic_confidential = driver.TrafficConfidential();
    return s;
  }

  [[nodiscard]] static kb::Result<Reply> status(const Dev& driver) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::NeedsReselect);
    const ::se05x::DeviceState state = driver.State();
    if (state == ::se05x::DeviceState::Dead) {
      result = Refuse<Reply>(::se05x::Error::TransportDead);
    } else if (state == ::se05x::DeviceState::Closed) {
      result = Refuse<Reply>(::se05x::Error::NotInitialized);
    } else if (IsLost(state)) {
      result = Refuse<Reply>(::se05x::Error::NeedsReselect);
    } else {
      Reply reply{};
      reply.status = statusOf(driver);
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> checkAlive(Dev& driver) noexcept {
    const ::se05x::Result<::se05x::AppletInfo> r = driver.GetVersion();
    kb::Result<Reply> result = Refuse<Reply>(r.error);
    if (r.ok() && (r.value.version.Packed() != driver.GetCapabilities().applet.Packed())) {
      result = Refuse<Reply>(::se05x::Error::DeviceIdMismatch); // another applet answers
    } else if (r.ok()) {
      Reply reply{};
      reply.status = statusOf(driver);
      reply.status.applet = r.value.version;
      result = kb::Result<Reply>::Success(reply);
    } else {
      // The driver's failure, as mapped above.
    }
    return result;
  }

  [[nodiscard]] static bool fits(uint16_t length, std::size_t capacity) noexcept {
    return (length != 0U) && (static_cast<std::size_t>(length) <= capacity);
  }

  [[nodiscard]] static kb::Result<Reply> random(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    if (fits(c.length, kMaxData)) {
      Reply reply{};
      const ::se05x::Result<void> r = driver.GetRandom(
          ::se05x::MutableByteSpan{reply.data}.first(static_cast<std::size_t>(c.length)));
      reply.length = c.length;
      result = r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> readObject(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    if (static_cast<std::size_t>(c.length) <= kMaxData) {
      Reply reply{};
      ::se05x::ReadObjectArgs args{};
      args.id = c.id;
      args.offset = c.offset;
      args.length = c.length;
      args.out = ::se05x::MutableByteSpan{reply.data};
      const ::se05x::Result<::se05x::LengthReply> r =
          driver.template Execute<::se05x::Command::ReadObject>(args);
      reply.length = static_cast<uint16_t>(r.value.length);
      result = r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> readSize(Dev& driver, const Command& c) noexcept {
    const ::se05x::Result<::se05x::U32Reply> r =
        driver.template Execute<::se05x::Command::ReadSize>(::se05x::IdArgs{c.id});
    Reply reply{};
    reply.value = r.value.value;
    return r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
  }

  [[nodiscard]] static kb::Result<Reply> exists(Dev& driver, const Command& c) noexcept {
    const ::se05x::Result<bool> r = driver.ObjectExists(c.id);
    Reply reply{};
    reply.flag = r.value;
    return r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
  }

  [[nodiscard]] static kb::Result<Reply> readAttested(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    if (static_cast<std::size_t>(c.length) <= kMaxData) {
      Reply reply{};
      std::array<uint8_t, kMaxSignature> signature{};
      ::se05x::ReadObjectAttestedArgs args{};
      args.id = c.id;
      args.offset = c.offset;
      args.length = c.length;
      args.attestation_key = c.attester.key;
      args.algo = c.attester.algo;
      args.data_out = ::se05x::MutableByteSpan{reply.data};
      args.signature_out = ::se05x::MutableByteSpan{signature};
      const ::se05x::Result<::se05x::attest::Checked> r = driver.ReadVerified(
          args, std::span<const uint8_t, kPointLength>{c.attester.trusted_public});
      reply.attestation.timestamp = r.value.timestamp;
      reply.attestation.unique_id = r.value.unique_id;
      reply.attestation.object_size = r.value.object_size;
      reply.attestation.length = static_cast<uint16_t>(r.value.data_length);
      reply.length = reply.attestation.length;
      result = r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> sign(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    if (fits(c.digest_length, kMaxDigest)) {
      Reply reply{};
      const ::se05x::Result<std::size_t> r = driver.EcdsaSign(
          c.sign.key, c.sign.algo,
          ::se05x::ByteSpan{c.digest}.first(static_cast<std::size_t>(c.digest_length)),
          ::se05x::MutableByteSpan{reply.data}.first(kMaxSignature));
      reply.length = static_cast<uint16_t>(r.value);
      result = r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> verifySignature(Dev& driver, const Command& c) noexcept {
    kb::Result<Reply> result = Refuse<Reply>(::se05x::Error::InvalidArgument);
    if (fits(c.digest_length, kMaxDigest) && fits(c.signature_length, kMaxSignature)) {
      Reply reply{};
      const ::se05x::Result<bool> r = driver.EcdsaVerify(
          c.sign.key, c.sign.algo,
          ::se05x::ByteSpan{c.digest}.first(static_cast<std::size_t>(c.digest_length)),
          ::se05x::ByteSpan{c.signature}.first(static_cast<std::size_t>(c.signature_length)));
      reply.flag = r.value;
      result = r.ok() ? kb::Result<Reply>::Success(reply) : Refuse<Reply>(r.error);
    }
    return result;
  }

  BusPort<Bus> bus_;
  ClockPort clock_;
  ResetPort reset_;
  Set ports_;
  Config config_;
  bool lost_{false};
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Copies the first `n` bytes of `from` into `to` (`n` at most both sizes). */
inline void CopyBytes(std::span<const uint8_t> from, std::span<uint8_t> to,
                      std::size_t n) noexcept {
  for (std::size_t i = 0U; i < n; ++i) {
    to[i] = from[i];
  }
}

/** @brief The device's state and lifecycle from the driver's memory (no I/O). */
template <typename Access>
[[nodiscard]] kb::Result<DeviceStatus> ReadStatus(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadStatus;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<DeviceStatus>::Success(r.Value().status) : Forward<DeviceStatus>(r);
}

/** @brief One GetVersion round trip: the chip answers, with the applet that was opened. */
template <typename Access>
[[nodiscard]] kb::Result<::se05x::AppletVersion> CheckAlive(Access& io,
                                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::CheckAlive;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::se05x::AppletVersion>::Success(r.Value().status.applet)
                  : Forward<::se05x::AppletVersion>(r);
}

/** @brief Fills `out` (1 to kMaxData bytes) from the chip's RNG. */
template <typename Access>
[[nodiscard]] kb::Result<void> GetRandom(Access& io, std::span<uint8_t> out,
                                         kb::Milliseconds timeout) noexcept {
  kb::Result<void> result = Refuse<void>(::se05x::Error::InvalidArgument);
  if ((!out.empty()) && (out.size() <= kMaxData)) {
    Command c{};
    c.op = Op::GetRandom;
    c.length = static_cast<uint16_t>(out.size());
    const kb::Result<Reply> r = io.Request(c, timeout);
    if (r.IsOk()) {
      CopyBytes(r.Value().data, out, out.size());
      result = kb::Result<void>::Success();
    } else {
      result = Forward<void>(r);
    }
  }
  return result;
}

/**
 * @brief Reads exactly `out.size()` bytes (1 to kMaxData) of object `id` from `offset`.
 * @return The bytes read.
 */
template <typename Access>
[[nodiscard]] kb::Result<std::size_t> ReadObject(Access& io, ::se05x::ObjectId id, uint16_t offset,
                                                 std::span<uint8_t> out, kb::Milliseconds timeout,
                                                 const Session& session = Session{}) noexcept {
  kb::Result<std::size_t> result = Refuse<std::size_t>(::se05x::Error::InvalidArgument);
  if ((!out.empty()) && (out.size() <= kMaxData)) {
    Command c{};
    c.op = Op::ReadObject;
    c.id = id;
    c.offset = offset;
    c.length = static_cast<uint16_t>(out.size());
    c.session = session;
    const kb::Result<Reply> r = io.Request(c, timeout);
    if (r.IsOk()) {
      const std::size_t n = (static_cast<std::size_t>(r.Value().length) < out.size())
                                ? static_cast<std::size_t>(r.Value().length)
                                : out.size();
      CopyBytes(r.Value().data, out, n);
      result = kb::Result<std::size_t>::Success(n);
    } else {
      result = Forward<std::size_t>(r);
    }
  }
  return result;
}

/**
 * @brief Reads a whole object (a certificate, a data file) into `out`: its size, then chunks of
 * kMaxData bytes, one call each (another task may run between them).
 * @return The object size; Overflow (BufferTooSmall in detail) when `out` is too small.
 */
template <typename Access>
[[nodiscard]] kb::Result<std::size_t> ReadObjectAll(Access& io, ::se05x::ObjectId id,
                                                    std::span<uint8_t> out,
                                                    kb::Milliseconds timeout,
                                                    const Session& session = Session{}) noexcept {
  Command c{};
  c.op = Op::ReadSize;
  c.id = id;
  c.session = session;
  const kb::Result<Reply> sized = io.Request(c, timeout);
  kb::Result<std::size_t> result = Forward<std::size_t>(sized);
  if (sized.IsOk()) {
    const std::size_t size = static_cast<std::size_t>(sized.Value().value);
    const std::size_t chunks = (size + kMaxData - 1U) / kMaxData;
    result = (size <= out.size()) ? kb::Result<std::size_t>::Success(size)
                                  : Refuse<std::size_t>(::se05x::Error::BufferTooSmall);
    for (std::size_t i = 0U; i < chunks; ++i) {
      const std::size_t at = i * kMaxData;
      const std::size_t n = ((size - at) < kMaxData) ? (size - at) : kMaxData;
      if (result.IsOk()) {
        const kb::Result<std::size_t> part =
            ReadObject(io, id, static_cast<uint16_t>(at), out.subspan(at, n), timeout, session);
        result = part.IsOk() ? result : part;
      }
    }
  }
  return result;
}

/** @brief The chip's 18-byte unique identifier (a plain read: use ReadAttested to trust it). */
template <typename Access>
[[nodiscard]] kb::Result<std::array<uint8_t, kUniqueIdLength>> ReadUniqueId(
    Access& io, kb::Milliseconds timeout) noexcept {
  std::array<uint8_t, kUniqueIdLength> uid{};
  const kb::Result<std::size_t> r =
      ReadObject(io, ::se05x::FromReserved(::se05x::ReservedId::UniqueId), 0U,
                 std::span<uint8_t>{uid}, timeout);
  using R = kb::Result<std::array<uint8_t, kUniqueIdLength>>;
  return r.IsOk() ? R::Success(uid) : Forward<std::array<uint8_t, kUniqueIdLength>>(r);
}

/**
 * @brief Reads the whole object `id` (at most kMaxData bytes: a public key, the unique id, a small
 * file) with an attestation, and returns it only when the signature, the fresh host nonce, the
 * timestamp order and the unique id verify against `attester.trusted_public` (AttestationInvalid
 * otherwise).
 * @return What the attestation proved, with the data length; Overflow (BufferTooSmall in detail)
 * when `out` is smaller than the object.
 */
template <typename Access>
[[nodiscard]] kb::Result<Attestation> ReadAttested(Access& io, ::se05x::ObjectId id,
                                                   const Attester& attester, std::span<uint8_t> out,
                                                   kb::Milliseconds timeout,
                                                   const Session& session = Session{}) noexcept {
  Command c{};
  c.op = Op::ReadAttested;
  c.id = id;
  c.attester = attester;
  c.session = session;
  const kb::Result<Reply> r = io.Request(c, timeout);
  const std::size_t got = r.IsOk() ? static_cast<std::size_t>(r.Value().attestation.length) : 0U;
  kb::Result<Attestation> result = Forward<Attestation>(r);
  if (!r.IsOk()) {
    // The handler's failure, as it came.
  } else if (got > out.size()) {
    result = Refuse<Attestation>(::se05x::Error::BufferTooSmall);
  } else {
    CopyBytes(r.Value().data, out, got);
    result = kb::Result<Attestation>::Success(r.Value().attestation);
  }
  return result;
}

/**
 * @brief ECDSA over `digest` (1 to kMaxDigest bytes) with key `with.key`; the DER signature goes
 * to `signature` (kMaxSignature bytes hold any curve).
 * @return The signature length; Overflow when `signature` is too small for it.
 */
template <typename Access>
[[nodiscard]] kb::Result<std::size_t> Sign(Access& io, const SignWith& with,
                                           std::span<const uint8_t> digest,
                                           std::span<uint8_t> signature, kb::Milliseconds timeout,
                                           const Session& session = Session{}) noexcept {
  kb::Result<std::size_t> result = Refuse<std::size_t>(::se05x::Error::InvalidArgument);
  if ((!digest.empty()) && (digest.size() <= kMaxDigest)) {
    Command c{};
    c.op = Op::Sign;
    c.sign = with;
    c.session = session;
    c.digest_length = static_cast<uint8_t>(digest.size());
    CopyBytes(digest, c.digest, digest.size());
    const kb::Result<Reply> r = io.Request(c, timeout);
    const std::size_t n = r.IsOk() ? static_cast<std::size_t>(r.Value().length) : 0U;
    if (!r.IsOk()) {
      result = Forward<std::size_t>(r);
    } else if (n > signature.size()) {
      result = Refuse<std::size_t>(::se05x::Error::BufferTooSmall);
    } else {
      CopyBytes(r.Value().data, signature, n);
      result = kb::Result<std::size_t>::Success(n);
    }
  }
  return result;
}

/**
 * @brief ECDSA verification of the DER `signature` over `digest` with key `with.key`.
 * @return true for a valid signature, false for an invalid one (both are a successful call).
 */
template <typename Access>
[[nodiscard]] kb::Result<bool> Verify(Access& io, const SignWith& with,
                                      std::span<const uint8_t> digest,
                                      std::span<const uint8_t> signature, kb::Milliseconds timeout,
                                      const Session& session = Session{}) noexcept {
  kb::Result<bool> result = Refuse<bool>(::se05x::Error::InvalidArgument);
  const bool sizes = (!digest.empty()) && (digest.size() <= kMaxDigest) && (!signature.empty()) &&
                     (signature.size() <= kMaxSignature);
  if (sizes) {
    Command c{};
    c.op = Op::Verify;
    c.sign = with;
    c.session = session;
    c.digest_length = static_cast<uint8_t>(digest.size());
    c.signature_length = static_cast<uint8_t>(signature.size());
    CopyBytes(digest, c.digest, digest.size());
    CopyBytes(signature, c.signature, signature.size());
    const kb::Result<Reply> r = io.Request(c, timeout);
    result = r.IsOk() ? kb::Result<bool>::Success(r.Value().flag) : Forward<bool>(r);
  }
  return result;
}

/// @}

} // namespace kb::handlers::se05x

#endif // KB_HANDLERS_BINDINGS_SE05X_HPP
