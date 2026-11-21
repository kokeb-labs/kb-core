/**
 * @file fdo2.hpp
 * @brief The fdo2 binding and handler: FDO2 and FDO2-G2 optical oxygen sensor modules on a UART.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Uart>`: the driver's UART hooks over a kb::iface::UartPort (UartHooks), `DelayUs`
 * and `NowUs` from the hook timing, `SetBaudRate` where the port can change its rate, and the
 * module's supply switch (GpioHooks; without the pin the board powers the module) | | Errors |
 * `ToStatus()`: every `fdo2::Error` to a kb::Status; the code in the health record's detail | |
 * Start-up | `Power` = `BeginInitialize()` (sync: validate, UART up, supply on); `Boot + Link +
 * Identify` = `Poll()` until initialised (polled: the power-up wait, `#VERS`, `#IDNR`) | | Thread
 * model | Owner by preference (`kRequiresOwner = false`): a UART is point to point, so its owner
 * task serves this module alone and a measurement that waits for its reply blocks nobody else; the
 * start is polled, so it never blocks the owner; a guarded port works too | | Health |
 * `DeviceFault` (a potentially fatal module error), `StateUnknown` (a flash write or baud change
 * without its echo) and `NotInitialized` fault at once; timeouts, CRC and line errors count to the
 * threshold | | App API | snapshot `{oxygen}` (`#MOXY` with the status checks); commands: oxygen,
 * raw reading, identify (LED), resync, power cycle, calibrations, broadcast, baud rate |
 *
 * @code
 * kb::handlers::BusDomain<Uart, kb::handlers::ThreadModel::Owner> uart2{uart};
 * kb::handlers::OwnerAccess<kb::handlers::fdo2::Handler<Uart>, decltype(uart2)> o2{uart2, uart,
 * timing, cfg}; o2.RequestStart();                                        // the owner task polls
 * the power-up const auto s = o2.Latest();                               // lock-free, newest
 * reading if (s.IsOk()) { use(s.Value().value.oxygen.o2_mhpa); }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_FDO2_HPP
#define KB_HANDLERS_BINDINGS_FDO2_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/handlers.hpp"
#include <fdo2.hpp>

namespace kb::handlers::fdo2 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::fdo2::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::fdo2::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::fdo2::Error::NotInitialized:
  case ::fdo2::Error::StateUnknown:
  case ::fdo2::Error::NotReady:
    s = kb::Status::NotReady;
    break;
  case ::fdo2::Error::InvalidArgument:
  case ::fdo2::Error::NotPermitted:
    s = kb::Status::InvalidArgument;
    break;
  case ::fdo2::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::fdo2::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::fdo2::Error::DeviceIdMismatch:
  case ::fdo2::Error::DeviceFault:
  case ::fdo2::Error::DeviceError:
  case ::fdo2::Error::InvalidReply:
  case ::fdo2::Error::EchoMismatch:
  case ::fdo2::Error::SensorFault:
    s = kb::Status::Fault;
    break;
  case ::fdo2::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::fdo2::Error::Busy:
  case ::fdo2::Error::BroadcastActive:
    s = kb::Status::Busy;
    break;
  case ::fdo2::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::fdo2::Error::OutOfRange:
  case ::fdo2::Error::CalibrationRejected:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the module's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::fdo2::Error error) noexcept {
  return (error == ::fdo2::Error::DeviceFault) || (error == ::fdo2::Error::StateUnknown) ||
         (error == ::fdo2::Error::NotInitialized);
}

/** @brief The control output index of the supply switch in the port's GpioHooks. */
inline constexpr std::size_t kPowerPin = 0U;

/**
 * @brief The driver's UART port over a kb::iface::UartPort, with the module's supply switch.
 * @tparam Uart The port (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Port : public ::fdo2::UartInterface<Port<Uart>>,
             public UartHooks<Port<Uart>, Uart>,
             public GpioHooks<Port<Uart>, 1U, 0U> {
public:
  /** @brief Over `uart` with `timing` and the optional supply switch; no I/O. */
  Port(Uart& uart, const HookTiming& timing, const OutputPin& power) noexcept
      : UartHooks<Port<Uart>, Uart>(uart, timing),
        GpioHooks<Port<Uart>, 1U, 0U>(std::array<OutputPin, 1U>{power},
                                      std::array<InputPin, 0U>{}) {}

  /** @brief Hook: UART up at the module's rate. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: send one command line. */
  bool Write(const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(data, len);
  }
  /**
   * @brief Hook: the bytes already received, 0 to `max`, waiting at most `timeout_ms` for the first
   * one: one byte with the timeout, then what is there without waiting.
   */
  std::size_t Read(uint8_t* data, std::size_t max, uint32_t timeout_ms) noexcept {
    std::size_t got = 0U;
    if ((data != nullptr) && (max != 0U)) {
      const std::span<uint8_t> all{data, max};
      got = this->BusRead(all.data(), 1U, timeout_ms);
      if ((got == 1U) && (max > 1U)) {
        const std::span<uint8_t> rest = all.subspan(1U);
        got += this->BusRead(rest.data(), rest.size(), 0U);
      }
    }
    return got;
  }
  /** @brief Hook: drop what was received and not read. */
  bool FlushInput() noexcept {
    return this->BusFlush();
  }
  /** @brief Hook: wait at least `us`. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: the free-running microsecond clock. */
  uint32_t NowUs() noexcept {
    return this->Timing().NowUs32();
  }
  /** @brief Optional hook: change the UART rate (present when the kb port can). */
  bool SetBaudRate(uint32_t baud) noexcept
    requires kb::iface::UartBaudControl<Uart>
  {
    return this->BusSetBaud(baud);
  }
  /**
   * @brief Optional hook: the module's supply. Without the pin the board powers the module, and
   * the call reports success (the driver then cannot cut the supply: `PowerCycle()` only restarts).
   */
  bool GpioSet(::fdo2::CtrlPin pin, ::fdo2::GpioSignal level) noexcept {
    bool ok = !this->HasOutput(kPowerPin);
    if ((pin == ::fdo2::CtrlPin::Power) && this->HasOutput(kPowerPin)) {
      ok = this->DriveOutput(kPowerPin, level == ::fdo2::GpioSignal::ACTIVE);
    }
    return ok;
  }
  /** @brief The UART counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return UartHooks<Port<Uart>, Uart>::Counters();
  }
};

/** @brief The driver type a handler over `Uart` owns. */
template <kb::iface::UartPort Uart>
using Driver = ::fdo2::FDO2<Port<Uart>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::fdo2::Config chip{};                ///< Family, rate, allowances, policies, plausibility.
  HandlerConfig handler{};              ///< Health, recovery, polling, sample period.
  kb::Milliseconds power_deadline{50U}; ///< Bound of the power stage.
  kb::Milliseconds boot_deadline{
      5000U};        ///< Bound of the boot and identification stage (power-up included).
  OutputPin power{}; ///< The module's supply switch; absent: the board powers it.
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::fdo2::Oxygen oxygen{}; ///< Oxygen partial pressure, temperature, status, warm-up flag.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  MeasureOxygen = 0,    ///< `#MOXY` now.
  MeasureRaw = 1,       ///< `#MRAW`: O, T, S and the raw signals, pressure, humidity.
  Identify = 2,         ///< `#LOGO`: flash the module's LED.
  Resync = 3,           ///< Find the module again after `StateUnknown`.
  PowerCycle = 4,       ///< Supply off, wait, on, start again (needs the pin).
  CalibrateZero = 5,    ///< Zero point (flash write): `steady_s` of steady state.
  CalibrateHigh = 6,    ///< High point at `po2_mhpa` (flash write).
  CalibrateAir = 7,     ///< High point in air at `ppm` oxygen (flash write).
  SetBroadcast = 8,     ///< Broadcast `#MRAW` every `interval_ms` (flash write).
  DisableBroadcast = 9, ///< Stop the broadcast (flash write).
  SetBaudRate = 10,     ///< `#BAUD` to `baud` and the UART with it.
};

/** @brief One command. */
struct Command {
  Op op{Op::MeasureOxygen};           ///< What to do.
  uint32_t steady_s{0U};              ///< Calibrations: seconds of steady state before the command.
  int32_t po2_mhpa{0};                ///< CalibrateHigh: the standard, 10^-3 hPa.
  uint32_t ppm{::fdo2::kDryAirO2Ppm}; ///< CalibrateAir: the oxygen fraction of the air.
  uint32_t interval_ms{0U};           ///< SetBroadcast.
  uint32_t baud{0U};                  ///< SetBaudRate.
};

/** @brief The reply of a command. */
struct Reply {
  ::fdo2::Raw raw{}; ///< MeasureRaw (its `oxygen` holds MeasureOxygen's reading too).
};

/**
 * @brief The fdo2 handler.
 * @tparam Uart The port (a UartPort or UartPortRef).
 */
template <kb::iface::UartPort Uart>
class Handler : public HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Uart>, Driver<Uart>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: power and the UART, then the polled power-up wait and identification.
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kPower, StageMode::Sync, false},
       {static_cast<uint16_t>(stage::kBoot | stage::kLink | stage::kIdentify), StageMode::Polled,
        false}}};
  static constexpr bool kRequiresOwner = false;  ///< Served by an owner loop or occasional calls.
  static constexpr BusKind kBus = BusKind::Uart; ///< On a UART.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Uart& uart, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(uart, timing, config.power), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Uart>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Uart>& driver) noexcept {
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Done);
    if (call.index == 0U) {
      const kb::Result<void> r = LiftStatus(driver.BeginInitialize(), &ToStatus);
      result = r.IsOk() ? result : Forward<StartProgress>(r);
    } else {
      const kb::Result<bool> r = Lift(driver.Poll(), &ToStatus);
      const bool done =
          r.IsOk() && driver.IsInitialized() && (driver.Pending() == ::fdo2::OperationKind::None);
      result = (!r.IsOk()) ? Forward<StartProgress>(r)
                           : kb::Result<StartProgress>::Success(done ? StartProgress::Done
                                                                     : StartProgress::Pending);
    }
    return result;
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.power_deadline : config_.boot_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Uart>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::fdo2::Error::DeviceFault)) ||
           (detail == DetailOf(::fdo2::Error::StateUnknown)) ||
           (detail == DetailOf(::fdo2::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Uart>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<::fdo2::Oxygen> r = Lift(driver.MeasureOxygen(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(r);
    if (r.IsOk()) {
      Snapshot s{};
      s.oxygen = r.Value();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Uart>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    Reply reply{};
    kb::Result<void> r = kb::Result<void>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::MeasureOxygen:
      r = take(driver.MeasureOxygen(), reply.raw.oxygen);
      break;
    case Op::MeasureRaw:
      r = take(driver.MeasureRaw(), reply.raw);
      break;
    case Op::Identify:
      r = LiftStatus(driver.Identify(), &ToStatus);
      break;
    case Op::Resync:
      r = LiftStatus(driver.Resync(), &ToStatus);
      break;
    case Op::PowerCycle:
      r = LiftStatus(driver.PowerCycle(), &ToStatus);
      break;
    case Op::CalibrateZero:
      r = LiftStatus(driver.CalibrateZero(c.steady_s), &ToStatus);
      break;
    case Op::CalibrateHigh:
      r = LiftStatus(driver.CalibrateHigh(c.po2_mhpa, c.steady_s), &ToStatus);
      break;
    case Op::CalibrateAir:
      r = LiftStatus(driver.CalibrateAir(c.steady_s, c.ppm), &ToStatus);
      break;
    case Op::SetBroadcast:
      r = LiftStatus(driver.SetBroadcast(c.interval_ms), &ToStatus);
      break;
    case Op::DisableBroadcast:
      r = LiftStatus(driver.DisableBroadcast(), &ToStatus);
      break;
    case Op::SetBaudRate:
      r = LiftStatus(driver.SetBaudRate(c.baud), &ToStatus);
      break;
    }
    return r.IsOk() ? kb::Result<Reply>::Success(reply) : Forward<Reply>(r);
  }
  /// @}

  /** @brief The port's UART counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  // A driver result's value into `out` (also on a failure: the driver's `T{}`), as
  // kb::Result<void>.
  template <typename T>
  [[nodiscard]] static kb::Result<void> take(const ::fdo2::Result<T>& r, T& out) noexcept {
    out = r.value;
    return LiftStatus(r, &ToStatus);
  }

  Port<Uart> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Runs `c` and drops the reply's values. */
template <typename Access>
[[nodiscard]] kb::Result<void> Send(Access& io, const Command& c,
                                    kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief One oxygen reading now (`#MOXY`, with the status and plausibility checks). */
template <typename Access>
[[nodiscard]] kb::Result<::fdo2::Oxygen> MeasureOxygen(Access& io,
                                                       kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::MeasureOxygen;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::fdo2::Oxygen>::Success(r.Value().raw.oxygen)
                  : Forward<::fdo2::Oxygen>(r);
}

/** @brief One raw reading now (`#MRAW`): the signals, pressure and humidity. */
template <typename Access>
[[nodiscard]] kb::Result<::fdo2::Raw> MeasureRaw(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::MeasureRaw;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::fdo2::Raw>::Success(r.Value().raw) : Forward<::fdo2::Raw>(r);
}

/** @brief Flashes the module's LED (`#LOGO`). */
template <typename Access>
[[nodiscard]] kb::Result<void> Identify(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Identify;
  return Send(io, c, timeout);
}

/** @brief Finds the module again after `StateUnknown` (candidate rates and CRC modes). */
template <typename Access>
[[nodiscard]] kb::Result<void> Resync(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Resync;
  return Send(io, c, timeout);
}

/** @brief Supply off, wait, on, and start again (the timeout must cover the power-up). */
template <typename Access>
[[nodiscard]] kb::Result<void> PowerCycle(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::PowerCycle;
  return Send(io, c, timeout);
}

/** @brief High-point calibration in air (a flash write; needs consent and a settled sensor). */
template <typename Access>
[[nodiscard]] kb::Result<void> CalibrateAir(Access& io, uint32_t steady_s, kb::Milliseconds timeout,
                                            uint32_t ppm = ::fdo2::kDryAirO2Ppm) noexcept {
  Command c{};
  c.op = Op::CalibrateAir;
  c.steady_s = steady_s;
  c.ppm = ppm;
  return Send(io, c, timeout);
}

/// @}

} // namespace kb::handlers::fdo2

#endif // KB_HANDLERS_BINDINGS_FDO2_HPP
