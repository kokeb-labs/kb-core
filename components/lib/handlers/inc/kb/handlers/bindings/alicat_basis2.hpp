/**
 * @file alicat_basis2.hpp
 * @brief The alicat_basis2 binding and handler: BASIS 2 mass flow meter or controller over a
 * serial line (ASCII protocol or Modbus RTU).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Uart>`: the driver's serial hooks over a kb::iface::UartPort (UartHooks): write
 * and drain, non-blocking read, flush, the millisecond clock, `DelayMs` and `SetBaud` | | Errors |
 * `ToStatus()`: every `alicat_basis2::Error` to a kb::Status; the code in the health record's
 * detail | | Start-up | `Link` = `driver.EnsureInitialized()` (sync: port up, configuration
 * checked, no traffic); `Identify` = `driver.Begin()` (sync: poll, firmware, serial number, full
 * scale, gas, controller settings; read only) | | Thread model | Guarded (`kRequiresOwner =
 * false`): a slow instrument whose transactions block up to a reply wait; an owner bus works too |
 * | Health | `StateUnknown` (a guarded command left the line settings in doubt) and
 * `NotInitialized` fault at once; other failures count to the threshold | | Recovery | The driver's
 * own `Recover()` (find the instrument again, adopt the ID that answers), then the plan from the
 * adopted configuration | | App API | snapshot `{frame}` (the data frame); commands: read frame,
 * setpoint, read setpoint, close, hold, resume valve, tare, gas, setpoint source, reset totalizer |
 *
 * @code
 * kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Guarded> line{uart};
 * kb::handlers::Guarded<kb::handlers::alicat_basis2::Handler<Uart>, decltype(line)> mfc{line, uart,
 * timing, cfg}; if (mfc.EnsureStarted(kb::Milliseconds{3000U}).IsOk()) {
 *   static_cast<void>(kb::handlers::alicat_basis2::SetSetpoint(mfc, 12.5, kb::Milliseconds{500U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_ALICAT_BASIS2_HPP
#define KB_HANDLERS_BINDINGS_ALICAT_BASIS2_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <alicat_basis2.hpp>

namespace kb::handlers::alicat_basis2 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::alicat_basis2::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::alicat_basis2::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::alicat_basis2::Error::NotInitialized:
  case ::alicat_basis2::Error::StateUnknown:
    s = kb::Status::NotReady;
    break;
  case ::alicat_basis2::Error::InvalidArgument:
  case ::alicat_basis2::Error::GuardNotConfirmed:
    s = kb::Status::InvalidArgument;
    break;
  case ::alicat_basis2::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::alicat_basis2::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::alicat_basis2::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::alicat_basis2::Error::Busy:
  case ::alicat_basis2::Error::RateLimited:
    s = kb::Status::Busy;
    break;
  case ::alicat_basis2::Error::Unsupported:
  case ::alicat_basis2::Error::FirmwareTooOld:
    s = kb::Status::Unsupported;
    break;
  case ::alicat_basis2::Error::OutOfRange:
    s = kb::Status::OutOfRange;
    break;
  case ::alicat_basis2::Error::Overflow:
    s = kb::Status::Overflow;
    break;
  case ::alicat_basis2::Error::DeviceIdMismatch:
  case ::alicat_basis2::Error::DeviceFault:
  case ::alicat_basis2::Error::BadReply:
  case ::alicat_basis2::Error::Rejected:
  case ::alicat_basis2::Error::VerifyMismatch:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the instrument's state is in doubt (fault at once).
 */
[[nodiscard]] constexpr bool IsFatalError(::alicat_basis2::Error error) noexcept {
  return (error == ::alicat_basis2::Error::StateUnknown) ||
         (error == ::alicat_basis2::Error::NotInitialized);
}

/**
 * @brief The driver's serial port over a kb::iface::UartPort.
 * @tparam Uart The port (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Port : public ::alicat_basis2::UartInterface<Port<Uart>>, public UartHooks<Port<Uart>, Uart> {
public:
  /** @brief Over `uart` with `timing`; `drain_ms` bounds the wait for the last stop bit. No I/O. */
  Port(Uart& uart, const HookTiming& timing, uint32_t drain_ms) noexcept
      : UartHooks<Port<Uart>, Uart>(uart, timing), drain_ms_(drain_ms) {}

  /** @brief Hook: bring the UART up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: send every byte and wait until the last one has left the wire. */
  bool Write(const uint8_t* data, std::size_t len) noexcept {
    const bool sent = this->BusWrite(data, len);
    return sent ? this->BusWaitTxDone(drain_ms_) : false;
  }
  /** @brief Hook: what arrived, waiting at most `timeout_ms` (the driver asks 0: never blocks). */
  std::size_t Read(uint8_t* out, std::size_t cap, uint32_t timeout_ms) noexcept {
    return this->BusRead(out, cap, timeout_ms);
  }
  /** @brief Hook: drop every byte not read yet. */
  bool FlushInput() noexcept {
    return this->BusFlush();
  }
  /** @brief Hook: the millisecond clock (wraps). */
  uint32_t NowMs() noexcept {
    return this->Timing().NowMs32();
  }
  /** @brief Optional hook: sleep instead of spinning on the clock. */
  void DelayMs(uint32_t ms) noexcept {
    HookTiming::DelayMs(ms);
  }
  /** @brief Optional hook: change the port's baud rate. */
  bool SetBaud(uint32_t baud) noexcept {
    return this->BusSetBaud(baud);
  }

private:
  uint32_t drain_ms_;
};

/** @brief The driver type a handler over `Uart` owns. */
template <kb::iface::UartPort Uart>
using Driver = ::alicat_basis2::Basis2<Port<Uart>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::alicat_basis2::Config chip{};            ///< Unit ID or Modbus address, protocol, baud, timing.
  HandlerConfig handler{};                   ///< Health, recovery, polling.
  kb::Milliseconds link_deadline{20U};       ///< Bound of the Link stage (no traffic).
  kb::Milliseconds identify_deadline{5000U}; ///< Bound of the Identify stage (Begin()).
  uint32_t drain_ms{20U};                    ///< Longest wait for the last byte to leave the wire.
  uint8_t devices_on_line{1U}; ///< Instruments on the serial line (Bus::SetDeviceCount).
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::alicat_basis2::Frame
      frame{}; ///< The data frame: flow, setpoint, temperature, total, gas, flags.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadFrame = 0,         ///< Poll the data frame now.
  SetSetpoint = 1,       ///< Setpoint to `value` (flow units; controllers).
  ReadSetpoint = 2,      ///< The current setpoint.
  CloseValve = 3,        ///< Hold the valve closed (never refused for StateUnknown).
  HoldValve = 4,         ///< Hold the valve at `value` percent.
  ResumeControl = 5,     ///< Closed-loop control again after a hold.
  Tare = 6,              ///< Tare the flow sensor for `duration_ms`.
  SetGas = 7,            ///< Select `gas`.
  SetSetpointSource = 8, ///< Setpoint source to `source`.
  ResetTotal = 9,        ///< Reset the totalizer.
};

/** @brief One command. */
struct Command {
  double value{0.0};            ///< Setpoint (flow units) or valve hold (percent).
  uint32_t duration_ms{1000U};  ///< For Tare.
  Op op{Op::ReadFrame};         ///< What to do.
  ::alicat_basis2::GasId gas{}; ///< For SetGas.
  ::alicat_basis2::SetpointSource source{
      ::alicat_basis2::SetpointSource::UnsavedDigital}; ///< For SetSetpointSource.
};

/** @brief The reply of a command. */
struct Reply {
  ::alicat_basis2::Frame frame{};   ///< ReadFrame: the frame.
  ::alicat_basis2::Decimal value{}; ///< ReadSetpoint: the setpoint.
};

/**
 * @brief The alicat_basis2 handler.
 * @tparam Uart The serial port (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Handler : public HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: the port and configuration check, then the read-only identification.
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kLink, StageMode::Sync, false}, {stage::kIdentify, StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false;  ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::Uart; ///< On a serial line.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Uart& uart, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(uart, timing, config.drain_ms), line_(port_), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Uart>>& slot) noexcept {
    line_.SetDeviceCount(config_.devices_on_line);
    slot.emplace(line_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Uart>& driver) noexcept {
    const kb::Result<void> r = (call.index == 0U)
                                   ? LiftStatus(driver.EnsureInitialized(), &ToStatus)
                                   : LiftStatus(driver.Begin(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.link_deadline : config_.identify_deadline;
  }
  [[nodiscard]] kb::Result<void> RecoverDriver(Driver<Uart>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<void> r = LiftStatus(driver.Recover(), &ToStatus);
    if (r.IsOk()) {
      config_.chip = driver.GetConfig(); // the ID, address or baud the instrument answered with
    }
    return r;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::alicat_basis2::Error::StateUnknown)) ||
           (detail == DetailOf(::alicat_basis2::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Uart>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<::alicat_basis2::Frame> frame = Lift(driver.ReadFrame(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(frame);
    if (frame.IsOk()) {
      Snapshot s{};
      s.frame = frame.Value();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Uart>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadFrame:
      result = frameReply(Lift(driver.ReadFrame(), &ToStatus));
      break;
    case Op::SetSetpoint:
      result = done(LiftStatus(driver.SetSetpoint(c.value), &ToStatus));
      break;
    case Op::ReadSetpoint:
      result = valueReply(Lift(driver.ReadSetpointValue(), &ToStatus));
      break;
    case Op::CloseValve:
      result = done(LiftStatus(driver.CloseValve(), &ToStatus));
      break;
    case Op::HoldValve:
      result = done(LiftStatus(driver.HoldValve(c.value), &ToStatus));
      break;
    case Op::ResumeControl:
      result = done(LiftStatus(driver.ResumeControl(), &ToStatus));
      break;
    case Op::Tare:
      result = done(LiftStatus(driver.Tare(c.duration_ms), &ToStatus));
      break;
    case Op::SetGas:
      result = done(LiftStatus(driver.SetGas(c.gas), &ToStatus));
      break;
    case Op::SetSetpointSource:
      result = done(LiftStatus(driver.SetSetpointSource(c.source), &ToStatus));
      break;
    case Op::ResetTotal:
      result = done(LiftStatus(driver.ResetTotal(), &ToStatus));
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
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> frameReply(
      const kb::Result<::alicat_basis2::Frame>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.frame = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> valueReply(
      const kb::Result<::alicat_basis2::Decimal>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Uart> port_;
  ::alicat_basis2::Bus<Port<Uart>> line_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief The data frame, polled now. */
template <typename Access>
[[nodiscard]] kb::Result<::alicat_basis2::Frame> ReadFrame(Access& io,
                                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFrame;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::alicat_basis2::Frame>::Success(r.Value().frame)
                  : Forward<::alicat_basis2::Frame>(r);
}

/** @brief The setpoint to `flow` in the instrument's flow unit (controllers). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetSetpoint(Access& io, double flow,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetSetpoint;
  c.value = flow;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The current setpoint (controllers). */
template <typename Access>
[[nodiscard]] kb::Result<::alicat_basis2::Decimal> ReadSetpoint(Access& io,
                                                                kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadSetpoint;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::alicat_basis2::Decimal>::Success(r.Value().value)
                  : Forward<::alicat_basis2::Decimal>(r);
}

/** @brief Hold the valve closed (controllers): the switch-off. */
template <typename Access>
[[nodiscard]] kb::Result<void> CloseValve(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::CloseValve;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Hold the valve at `percent` of full drive (controllers, ASCII). */
template <typename Access>
[[nodiscard]] kb::Result<void> HoldValve(Access& io, double percent,
                                         kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::HoldValve;
  c.value = percent;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Closed-loop control again after a hold (controllers). */
template <typename Access>
[[nodiscard]] kb::Result<void> ResumeControl(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ResumeControl;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Tare the flow sensor for `duration_ms` (pure process gas, true zero flow). */
template <typename Access>
[[nodiscard]] kb::Result<void> Tare(Access& io, uint32_t duration_ms,
                                    kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Tare;
  c.duration_ms = duration_ms;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Select the gas. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetGas(Access& io, ::alicat_basis2::GasId gas,
                                      kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetGas;
  c.gas = gas;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::alicat_basis2

#endif // KB_HANDLERS_BINDINGS_ALICAT_BASIS2_HPP
