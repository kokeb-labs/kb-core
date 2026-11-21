// clang-format off: the table below must keep one row per line.
/**
 * @file as5047u.hpp
 * @brief The as5047u binding and handler: AS5047U 14-bit SPI magnetic angle encoder.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks), with the required `DelayUs` through the hook timing (a spin below 1 ms, a sleep from 1 ms) |
 * | Errors | `ToStatus()`: every `as5047u::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Identify + Configure` = `driver.EnsureInitialized()` (sync): DIA polled until the loops settle (`Config::chip.startup_timeout_ms`), ERRFL cleared, every setting written and read back |
 * | Thread model | Owner task recommended (`kRequiresOwner = false`): an encoder is sampled at the control rate and read lock-free with `Latest()`; a guarded bus works for slow use |
 * | Health | `DeviceReset` (the chip reloaded its settings) and `NotInitialized` fault at once; `MagnetWarning`, bus and CRC errors count to the threshold |
 * | App API | snapshot `{angle, velocity, agc, magnitude, latched ERRFL}`; commands: angle, velocity, zero here, zero position, direction, diagnostics, error flags, reset check, raw registers |
 *
 * The one-time OTP burn (`ProgramOtp()`) is deliberately not a handler command: it cannot be
 * undone and belongs in a production tool that owns the driver directly.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Owner> spi1{dev};
 * kb::handlers::OwnerAccess<kb::handlers::as5047u::Handler<Dev>, decltype(spi1)> enc{spi1, dev, timing, cfg};
 * // the owner task calls spi1.Service(now, enc) every control step
 * const auto snap = enc.Latest();  // lock-free: angle and velocity of the newest sample
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_AS5047U_HPP
#define KB_HANDLERS_BINDINGS_AS5047U_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <as5047u.hpp>

namespace kb::handlers::as5047u {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::as5047u::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::as5047u::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::as5047u::Error::NotInitialized:
  case ::as5047u::Error::NotReady:
  case ::as5047u::Error::DeviceReset:
    s = kb::Status::NotReady;
    break;
  case ::as5047u::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::as5047u::Error::Bus:
  case ::as5047u::Error::Framing:
  case ::as5047u::Error::Command:
    s = kb::Status::BusError;
    break;
  case ::as5047u::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::as5047u::Error::DeviceIdMismatch:
  case ::as5047u::Error::DeviceFault:
  case ::as5047u::Error::WriteMismatch:
  case ::as5047u::Error::OtpVerify:
    s = kb::Status::Fault;
    break;
  case ::as5047u::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::as5047u::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::as5047u::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::as5047u::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  case ::as5047u::Error::MagnetWarning:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::as5047u::Error error) noexcept {
  return (error == ::as5047u::Error::DeviceReset) || (error == ::as5047u::Error::NotInitialized);
}

/**
 * @brief The driver's SPI port over a kb::iface::SpiDevice.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef): mode 1, at most `as5047u::kMaxClockHz`.
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::as5047u::SpiInterface<Port<Dev>>, public SpiHooks<Port<Dev>, Dev> {
public:
  /** @brief Over `dev` with `timing`; no I/O. */
  Port(Dev& dev, const HookTiming& timing) noexcept : SpiHooks<Port<Dev>, Dev>(dev, timing) {}

  /** @brief Hook: bring the device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one full-duplex frame. */
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return this->BusTransfer(tx, rx, len);
  }
  /**
   * @brief Hook: wait at least `us` microseconds. A sleep (rounded up to the scheduler tick), not
   * the hook timing's spin: HookTiming keeps no valid spin reference (see the doc page).
   */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::as5047u::AS5047U<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::as5047u::Config chip{};             ///< What EnsureInitialized() writes.
  HandlerConfig handler{};              ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{50U}; ///< Bound of the start-up stage (covers DIA polling).
  bool sample_velocity{true};           ///< Measure() also reads the velocity (2 more frames).
  bool sample_field{false};             ///< Measure() also reads AGC and magnitude (4 more frames).
};

/** @brief What a sample publishes. */
struct Snapshot {
  uint16_t angle_counts{0U};  ///< Angle at 0x3FFF, 0 to 16383 (DAEC unless configured off).
  int16_t velocity_counts{0}; ///< Signed 14-bit velocity (24.141 deg/s per count); 0 when off.
  uint16_t magnitude{0U};     ///< CORDIC magnitude; 0 unless `sample_field`.
  uint8_t agc{0U};            ///< AGC, low for a strong field; 0 unless `sample_field`.
  uint16_t latched_errfl{0U}; ///< ERRFL flags the driver latched since the last clear (raw).
  bool flags_lost{false};     ///< An ERRFL read failed: flags may have been missed.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadAngle = 0,          ///< Angle counts now (`Reply::value`).
  ReadVelocity = 1,       ///< Velocity counts now (`Reply::signed_value`).
  ReadAgc = 2,            ///< AGC now (`Reply::value`).
  ReadMagnitude = 3,      ///< CORDIC magnitude now (`Reply::value`).
  ReadDiagnostics = 4,    ///< DIA, raw (`Reply::value`).
  ReadErrorFlags = 5,     ///< ERRFL, raw; the read clears it on the chip (`Reply::value`).
  CheckForReset = 6,      ///< Compare the settings registers with what the driver wrote.
  SetZeroHere = 7,        ///< Make the current position read 0 (`Reply::value`: the new zero).
  SetZeroPosition = 8,    ///< Zero position to `value` counts.
  SetDirection = 9,       ///< Direction to `direction`.
  ClearLatchedFlags = 10, ///< Empty the driver's ERRFL latch (no I/O).
  ReadRegister = 11,      ///< Register `value` once (`Reply::value`).
  WriteRegister = 12,     ///< Register `value` to `data`.
};

/** @brief One command. */
struct Command {
  uint16_t value{0U};                                              ///< Counts or register address.
  uint16_t data{0U};                                               ///< For WriteRegister.
  Op op{Op::ReadAngle};                                            ///< What to do.
  ::as5047u::Direction direction{::as5047u::Direction::Clockwise}; ///< For SetDirection.
  ::as5047u::Verify verify{::as5047u::Verify::None};               ///< Read back after writing.
};

/** @brief The reply of a command. */
struct Reply {
  uint16_t value{0U};      ///< The unsigned value the reads and SetZeroHere return; 0 otherwise.
  int16_t signed_value{0}; ///< The velocity of ReadVelocity; 0 otherwise.
};

/**
 * @brief The as5047u handler.
 * @tparam Dev The SPI device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Sampled snapshots; occasional calls work too.
  static constexpr BusKind kBus = BusKind::Spi; ///< On an SPI device.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Dev& dev, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(dev, timing), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Dev>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Dev>& driver) noexcept {
    static_cast<void>(call);
    const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Dev>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::as5047u::Error::DeviceReset)) ||
           (detail == DetailOf(::as5047u::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    Snapshot s{};
    kb::Result<void> r = readInto(Lift(driver.ReadAngleCounts(), &ToStatus), s.angle_counts);
    if (r.IsOk() && config_.sample_velocity) {
      r = readInto(Lift(driver.ReadVelocityCounts(), &ToStatus), s.velocity_counts);
    }
    if (r.IsOk() && config_.sample_field) {
      r = readInto(Lift(driver.ReadAgc(), &ToStatus), s.agc);
    }
    if (r.IsOk() && config_.sample_field) {
      r = readInto(Lift(driver.ReadMagnitude(), &ToStatus), s.magnitude);
    }
    const ::as5047u::ErrorFlags latched = driver.GetLatchedErrorFlags();
    s.latched_errfl = latched.raw;
    s.flags_lost = latched.flags_lost;
    return r.IsOk() ? kb::Result<Snapshot>::Success(s) : Forward<Snapshot>(r);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadAngle:
      result = value(Lift(driver.ReadAngleCounts(), &ToStatus));
      break;
    case Op::ReadVelocity:
      result = velocity(Lift(driver.ReadVelocityCounts(), &ToStatus));
      break;
    case Op::ReadAgc:
      result = small(Lift(driver.ReadAgc(), &ToStatus));
      break;
    case Op::ReadMagnitude:
      result = value(Lift(driver.ReadMagnitude(), &ToStatus));
      break;
    case Op::ReadDiagnostics:
      result = diagnostics(Lift(driver.ReadDiagnostics(), &ToStatus));
      break;
    case Op::ReadErrorFlags:
      result = errorFlags(Lift(driver.ReadErrorFlags(), &ToStatus));
      break;
    case Op::CheckForReset:
      result = done(LiftStatus(driver.CheckForReset(), &ToStatus));
      break;
    case Op::SetZeroHere:
      result = value(Lift(driver.SetZeroHere(c.verify), &ToStatus));
      break;
    case Op::SetZeroPosition:
      result = done(LiftStatus(driver.SetZeroPosition(c.value, c.verify), &ToStatus));
      break;
    case Op::SetDirection:
      result = done(LiftStatus(driver.SetDirection(c.direction, c.verify), &ToStatus));
      break;
    case Op::ClearLatchedFlags:
      driver.ClearLatchedErrorFlags();
      result = kb::Result<Reply>::Success(Reply{});
      break;
    case Op::ReadRegister:
      result = value(Lift(driver.ReadRegister(c.value), &ToStatus));
      break;
    case Op::WriteRegister:
      result = done(LiftStatus(driver.WriteRegister(c.value, c.data, c.verify), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.Counters();
  }

private:
  template <typename T>
  [[nodiscard]] static kb::Result<void> readInto(const kb::Result<T>& r, T& out) noexcept {
    out = r.ValueOr(T{});
    return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
  }
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> value(const kb::Result<uint16_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> small(const kb::Result<uint8_t>& r) noexcept {
    return r.IsOk() ? value(kb::Result<uint16_t>::Success(static_cast<uint16_t>(r.Value())))
                    : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> velocity(const kb::Result<int16_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.signed_value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> diagnostics(
      const kb::Result<::as5047u::Diagnostics>& r) noexcept {
    return r.IsOk() ? value(kb::Result<uint16_t>::Success(r.Value().raw)) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> errorFlags(
      const kb::Result<::as5047u::ErrorFlags>& r) noexcept {
    return r.IsOk() ? value(kb::Result<uint16_t>::Success(r.Value().raw)) : Forward<Reply>(r);
  }

  Port<Dev> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief The angle in counts (0 to 16383), read now. */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> ReadAngle(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadAngle;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/** @brief The velocity in counts (24.141 deg/s per count), read now. */
template <typename Access>
[[nodiscard]] kb::Result<int16_t> ReadVelocity(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadVelocity;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int16_t>::Success(r.Value().signed_value) : Forward<int16_t>(r);
}

/** @brief Make the current position read as 0; returns the new zero position in counts. */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> SetZeroHere(
    Access& io, kb::Milliseconds timeout,
    ::as5047u::Verify verify = ::as5047u::Verify::None) noexcept {
  Command c{};
  c.op = Op::SetZeroHere;
  c.verify = verify;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/** @brief The direction in which the angle increases. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetDirection(Access& io, ::as5047u::Direction direction,
                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetDirection;
  c.direction = direction;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Check that the chip still holds the driver's settings (a reset faults the handler). */
template <typename Access>
[[nodiscard]] kb::Result<void> CheckForReset(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::CheckForReset;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief ERRFL read now (raw bits); the read clears it on the chip. */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> ReadErrorFlags(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadErrorFlags;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/// @}

} // namespace kb::handlers::as5047u

#endif // KB_HANDLERS_BINDINGS_AS5047U_HPP
