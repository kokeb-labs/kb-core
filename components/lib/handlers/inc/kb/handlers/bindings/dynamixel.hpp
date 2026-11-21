/**
 * @file dynamixel.hpp
 * @brief The dynamixel binding and handler: smart servos (protocol 1.0 or 2.0) on one half-duplex
 * UART bus, several IDs per bus.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Uart>`: the driver's UART hooks over a kb::iface::UartPort (UartHooks): write and
 * drain, a read that returns as soon as one byte is there, flush, `DelayUs` and `SetBaudRate` | |
 * Errors | `ToStatus()`: every `dynamixel::Error` to a kb::Status; the code in the health record's
 * detail | | Start-up | `Link` = `driver.EnsureInitialized()` (sync: configuration checked, port
 * up, baud applied); `Identify` = one `Identify(id)` per configured servo (polled, one servo per
 * step, optional: skipped with no servo listed), the family checked against the configuration | |
 * Thread model | Owner recommended (a servo loop is periodic), guarded allowed (`kRequiresOwner =
 * false`): the servos only answer when asked | | Health | `LineFault` (a stuck or floating line)
 * and `NotInitialized` fault at once; other failures count to the threshold | | App API | snapshot
 * `{feedback per listed servo, alert mask}`; commands: torque, goal position, feedback, register
 * read and write, ping, reboot, torque off for a family, resync |
 *
 * One handler serves one bus: every servo on it is addressed by its `ServoRef` (ID and family).
 *
 * @code
 * kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Owner> arm{uart};
 * kb::handlers::OwnerAccess<kb::handlers::dynamixel::Handler<Uart>, decltype(arm)> servos{arm,
 * uart, timing, cfg};
 * // owner task: arm.Service(now, servos) every step
 * static_cast<void>(kb::handlers::dynamixel::SetTorque(servos, {1U, dynamixel::Family::X430}, true,
 * kb::Milliseconds{20U}));
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_DYNAMIXEL_HPP
#define KB_HANDLERS_BINDINGS_DYNAMIXEL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/handlers.hpp"
#include <dynamixel.hpp>

namespace kb::handlers::dynamixel {

/** @brief The most servos a handler lists for its start-up check and its periodic read. */
inline constexpr std::size_t kMaxServos = 8U;

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::dynamixel::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::dynamixel::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::dynamixel::Error::NotInitialized:
    s = kb::Status::NotReady;
    break;
  case ::dynamixel::Error::InvalidArgument:
  case ::dynamixel::Error::NotPermitted:
    s = kb::Status::InvalidArgument;
    break;
  case ::dynamixel::Error::Bus:
  case ::dynamixel::Error::EchoMismatch:
  case ::dynamixel::Error::LineFault:
    s = kb::Status::BusError;
    break;
  case ::dynamixel::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::dynamixel::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::dynamixel::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::dynamixel::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::dynamixel::Error::DeviceIdMismatch:
  case ::dynamixel::Error::DeviceFault:
  case ::dynamixel::Error::DeviceError:
  case ::dynamixel::Error::InvalidReply:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the bus cannot be trusted (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::dynamixel::Error error) noexcept {
  return (error == ::dynamixel::Error::LineFault) || (error == ::dynamixel::Error::NotInitialized);
}

/**
 * @brief The driver's UART port over a kb::iface::UartPort.
 * @tparam Uart The port (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Port : public ::dynamixel::UartInterface<Port<Uart>>, public UartHooks<Port<Uart>, Uart> {
public:
  /** @brief Over `uart` with `timing`; `drain_ms` bounds the wait for the last stop bit. No I/O. */
  Port(Uart& uart, const HookTiming& timing, uint32_t drain_ms) noexcept
      : UartHooks<Port<Uart>, Uart>(uart, timing), drain_ms_(drain_ms) {}

  /** @brief Hook: bring the UART up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: send the packet in one go and wait until its last bit has left the wire. */
  bool Write(const uint8_t* data, std::size_t len) noexcept {
    const bool sent = this->BusWrite(data, len);
    return sent ? this->BusWaitTxDone(drain_ms_) : false;
  }
  /** @brief Hook: 1 to `max` bytes as soon as one is there, waiting at most `timeout_us` for it. */
  std::size_t Read(uint8_t* data, std::size_t max, uint32_t timeout_us) noexcept {
    std::size_t got = 0U;
    if ((data != nullptr) && (max != 0U)) {
      const std::span<uint8_t> all{data, max};
      const uint32_t wait_ms = kb::utils::ToMillisecondsCeil(kb::Microseconds{timeout_us}).Count();
      got = this->BusRead(all.data(), 1U, wait_ms);
      if ((got == 1U) && (max > 1U)) {
        got += this->BusRead(all.subspan(1U).data(), max - 1U, 0U);
      }
    }
    return got;
  }
  /** @brief Hook: drop every byte not read yet. */
  bool FlushInput() noexcept {
    return this->BusFlush();
  }
  /**
   * @brief Hook: wait at least `us` microseconds (the hook timing: a spin below 1 ms, a sleep
   * from 1 ms).
   */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: reconfigure the UART's baud rate. */
  bool SetBaudRate(uint32_t baud) noexcept {
    return this->BusSetBaud(baud);
  }

private:
  uint32_t drain_ms_;
};

/** @brief The driver type a handler over `Uart` owns. */
template <kb::iface::UartPort Uart>
using Driver = ::dynamixel::DYNAMIXEL<Port<Uart>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::dynamixel::Config chip{};               ///< Protocol, baud, reply timing, policies.
  HandlerConfig handler{};                  ///< Health, recovery, polling.
  kb::Milliseconds link_deadline{50U};      ///< Bound of the Link stage.
  kb::Milliseconds identify_deadline{500U}; ///< Bound of the Identify stage (all servos).
  uint32_t drain_ms{10U};                   ///< Longest wait for the last byte to leave.
  std::array<::dynamixel::ServoRef, kMaxServos> servos{}; ///< The servos checked and sampled.
  uint8_t servo_count{0U};                                ///< Entries of `servos` in use.
  bool check_family{true}; ///< Identify fails when a listed family differs from the model's.
};

/** @brief What a sample publishes: the feedback of every listed servo, in list order. */
struct Snapshot {
  std::array<::dynamixel::Feedback, kMaxServos> feedback{}; ///< Position, velocity, effort, ...
  uint8_t count{0U};                                        ///< Entries of `feedback` filled.
  uint8_t alert_mask{0U}; ///< Bit n: servo n reported a hardware alert in its reply.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SetTorque = 0,        ///< Torque of `servo` on (`enable`) or off.
  SetGoalPosition = 1,  ///< Goal position of `servo` to `value` counts.
  ReadFeedback = 2,     ///< Feedback of `servo` now.
  WriteRegister = 3,    ///< `reg` of `servo` to `value`.
  ReadRegister = 4,     ///< `reg` of `servo`.
  Ping = 5,             ///< Ping and identify `servo.id`.
  Reboot = 6,           ///< Reboot `servo.id`.
  DisableTorqueAll = 7, ///< Torque off for every servo of `servo.family` (one broadcast).
  Resync = 8,           ///< Wait for a quiet line and drop what is buffered.
};

/** @brief One command. */
struct Command {
  ::dynamixel::ServoRef servo{};                        ///< The servo (ID and family).
  int32_t value{0};                                     ///< Goal position or register value.
  ::dynamixel::Reg reg{::dynamixel::Reg::GoalPosition}; ///< For the register calls.
  Op op{Op::ReadFeedback};                              ///< What to do.
  bool enable{false};                                   ///< For SetTorque.
};

/** @brief The reply of a command. */
struct Reply {
  ::dynamixel::Feedback feedback{}; ///< ReadFeedback.
  ::dynamixel::PingInfo ping{};     ///< Ping.
  int32_t value{0};                 ///< ReadRegister.
};

/**
 * @brief The dynamixel handler: one bus of servos.
 * @tparam Uart The UART (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Handler : public HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: the port up, then each listed servo identified (polled, optional).
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kLink, StageMode::Sync, false}, {stage::kIdentify, StageMode::Polled, true}}};
  static constexpr bool kRequiresOwner = false;  ///< The servos only answer when asked.
  static constexpr BusKind kBus = BusKind::Uart; ///< On a UART bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Uart& uart, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(uart, timing, config.drain_ms), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Uart>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept {
    return (index == 0U) || (servoCount() != 0U);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Uart>& driver) noexcept {
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Done);
    if (call.index == 0U) {
      const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
      result = r.IsOk() ? result : Forward<StartProgress>(r);
    } else {
      result = identifyNext(driver, static_cast<std::size_t>(call.calls));
    }
    return result;
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.link_deadline : config_.identify_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Uart>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::dynamixel::Error::LineFault)) ||
           (detail == DetailOf(::dynamixel::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Uart>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    Snapshot s{};
    kb::Result<void> failure = kb::Result<void>::Success();
    for (std::size_t i = 0U; i < servoCount(); ++i) {
      if (failure.IsOk()) {
        const kb::Result<::dynamixel::Feedback> fb =
            Lift(driver.ReadFeedback(config_.servos[i]), &ToStatus);
        failure = fb.IsOk() ? failure : Forward<void>(fb);
        s.feedback[i] = fb.ValueOr(::dynamixel::Feedback{});
        const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint32_t>(i));
        s.alert_mask =
            driver.GetLastReply().alert ? static_cast<uint8_t>(s.alert_mask | bit) : s.alert_mask;
      }
    }
    s.count = static_cast<uint8_t>(servoCount());
    return failure.IsOk() ? kb::Result<Snapshot>::Success(s) : Forward<Snapshot>(failure);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Uart>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SetTorque:
      result = done(LiftStatus(driver.SetTorque(c.servo, c.enable), &ToStatus));
      break;
    case Op::SetGoalPosition:
      result = done(LiftStatus(driver.SetGoalPosition(c.servo, c.value), &ToStatus));
      break;
    case Op::ReadFeedback:
      result = feedbackReply(Lift(driver.ReadFeedback(c.servo), &ToStatus));
      break;
    case Op::WriteRegister:
      result = done(LiftStatus(driver.WriteRegister(c.servo, c.reg, c.value), &ToStatus));
      break;
    case Op::ReadRegister:
      result = valueReply(Lift(driver.ReadRegister(c.servo, c.reg), &ToStatus));
      break;
    case Op::Ping:
      result = pingReply(Lift(driver.Identify(c.servo.id), &ToStatus));
      break;
    case Op::Reboot:
      result = done(LiftStatus(driver.Reboot(c.servo.id), &ToStatus));
      break;
    case Op::DisableTorqueAll:
      result = done(LiftStatus(driver.DisableTorqueAll(c.servo.family), &ToStatus));
      break;
    case Op::Resync:
      result = done(LiftStatus(driver.Resync(), &ToStatus));
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
  [[nodiscard]] std::size_t servoCount() const noexcept {
    const std::size_t n = static_cast<std::size_t>(config_.servo_count);
    return (n > kMaxServos) ? kMaxServos : n;
  }

  // One step of the Identify stage: the servo of this step, its family checked.
  [[nodiscard]] kb::Result<StartProgress> identifyNext(Driver<Uart>& driver,
                                                       std::size_t step) noexcept {
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Done);
    if (step < servoCount()) {
      const ::dynamixel::ServoRef& want = config_.servos[step];
      const kb::Result<::dynamixel::PingInfo> info = Lift(driver.Identify(want.id), &ToStatus);
      const bool listed = want.family != ::dynamixel::Family::Generic;
      const bool same = info.IsOk() && (info.Value().family == want.family);
      if (!info.IsOk()) {
        result = Forward<StartProgress>(info);
      } else if (config_.check_family && listed && (!same)) {
        result = kb::Result<StartProgress>::Failure(kb::Status::Fault,
                                                    DetailOf(::dynamixel::Error::DeviceIdMismatch));
      } else if ((step + 1U) < servoCount()) {
        result = kb::Result<StartProgress>::Success(StartProgress::Pending);
      } else {
        // The last listed servo answered: the stage is done.
      }
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> feedbackReply(
      const kb::Result<::dynamixel::Feedback>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.feedback = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> pingReply(
      const kb::Result<::dynamixel::PingInfo>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.ping = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> valueReply(const kb::Result<int32_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Uart> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief The torque of `servo` on or off. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetTorque(Access& io, ::dynamixel::ServoRef servo, bool enable,
                                         kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetTorque;
  c.servo = servo;
  c.enable = enable;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The goal position of `servo`, in counts. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetGoalPosition(Access& io, ::dynamixel::ServoRef servo,
                                               int32_t counts, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetGoalPosition;
  c.servo = servo;
  c.value = counts;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The feedback of `servo`, read now. */
template <typename Access>
[[nodiscard]] kb::Result<::dynamixel::Feedback> ReadFeedback(Access& io,
                                                             ::dynamixel::ServoRef servo,
                                                             kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFeedback;
  c.servo = servo;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::dynamixel::Feedback>::Success(r.Value().feedback)
                  : Forward<::dynamixel::Feedback>(r);
}

/** @brief A typed register of `servo` to `value`. */
template <typename Access>
[[nodiscard]] kb::Result<void> WriteRegister(Access& io, ::dynamixel::ServoRef servo,
                                             ::dynamixel::Reg reg, int32_t value,
                                             kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::WriteRegister;
  c.servo = servo;
  c.reg = reg;
  c.value = value;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief A typed register of `servo`. */
template <typename Access>
[[nodiscard]] kb::Result<int32_t> ReadRegister(Access& io, ::dynamixel::ServoRef servo,
                                               ::dynamixel::Reg reg,
                                               kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadRegister;
  c.servo = servo;
  c.reg = reg;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int32_t>::Success(r.Value().value) : Forward<int32_t>(r);
}

/** @brief Ping and identify servo `id`. */
template <typename Access>
[[nodiscard]] kb::Result<::dynamixel::PingInfo> Ping(Access& io, uint8_t id,
                                                     kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Ping;
  c.servo.id = id;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::dynamixel::PingInfo>::Success(r.Value().ping)
                  : Forward<::dynamixel::PingInfo>(r);
}

/** @brief Torque off for every servo of `family` with one broadcast: the bus stop. */
template <typename Access>
[[nodiscard]] kb::Result<void> DisableTorqueAll(Access& io, ::dynamixel::Family family,
                                                kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisableTorqueAll;
  c.servo.family = family;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::dynamixel

#endif // KB_HANDLERS_BINDINGS_DYNAMIXEL_HPP
