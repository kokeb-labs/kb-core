/**
 * @file pf1550.hpp
 * @brief The pf1550 binding and handler: PF1550 power management IC (three bucks, three LDOs, a
 * linear charger and an interrupt controller) on I2C.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Bus>`: the driver's I2C hooks over a kb::iface::I2cBus (I2cHooks) with
 * `GetLastFailure` (an absent chip is `NoDevice`) and `DelayUs`; STANDBY and INTB through GpioHooks
 * (`GpioSet`, `GpioGet`), each absent unless the HAL supplies it | | Errors | `ToStatus()`: every
 * `pf1550::Error` to a kb::Status; the code in the health record's detail | | Start-up | `Identify`
 * = `EnsureInitialized()` (sync: DEVICE_ID, option, charger as found against `Limits`); `Configure`
 * = `Configure()` (sync: rails, power control, masks, charger last) | | Thread model | Guarded
 * (`kRequiresOwner = false`): a power IC is used sporadically from several tasks (power manager,
 * charger policy, the task an INTB interrupt notifies); an owner bus works too | | Health |
 * `DeviceReset` (the chip lost the registers the driver wrote) and `NotInitialized` fault at once;
 * refusals (`OutOfLimits`, `Unsafe`, `Refused`) and bus failures count to the threshold | | App API
 * | snapshot `{charger, state, events, intb}`; commands: charger mode, restart, watchdog, rails,
 * standby, interrupts, self check, shutdown cancel, interrupt masks, one register |
 *
 * `EnterShip()` and `EnterCoreOff()` cut the supplies of the system and take confirmation tokens;
 * they are not commands of the handler (call the driver from the code that owns the shutdown).
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c0{bus};
 * kb::handlers::Guarded<kb::handlers::pf1550::Handler<Bus>, decltype(i2c0)> pmic{i2c0, bus, timing,
 * cfg}; if (pmic.EnsureStarted(kb::Milliseconds{50U}).IsOk()) {
 *   static_cast<void>(kb::handlers::pf1550::SetChargerMode(pmic, pf1550::ChargerMode::Charge,
 * kb::Milliseconds{10U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_PF1550_HPP
#define KB_HANDLERS_BINDINGS_PF1550_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <pf1550.hpp>

namespace kb::handlers::pf1550 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::pf1550::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::pf1550::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::pf1550::Error::NotInitialized:
  case ::pf1550::Error::DeviceReset:
  case ::pf1550::Error::Refused:
    s = kb::Status::NotReady;
    break;
  case ::pf1550::Error::InvalidArgument:
  case ::pf1550::Error::Unsafe:
    s = kb::Status::InvalidArgument;
    break;
  case ::pf1550::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::pf1550::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::pf1550::Error::DeviceIdMismatch:
  case ::pf1550::Error::DeviceFault:
  case ::pf1550::Error::ChargerFault:
  case ::pf1550::Error::VerifyFailed:
    s = kb::Status::Fault;
    break;
  case ::pf1550::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::pf1550::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::pf1550::Error::Unsupported:
  case ::pf1550::Error::ReadOnlyRail:
    s = kb::Status::Unsupported;
    break;
  case ::pf1550::Error::OutOfLimits:
    s = kb::Status::OutOfRange;
    break;
  case ::pf1550::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::pf1550::Error error) noexcept {
  return (error == ::pf1550::Error::DeviceReset) || (error == ::pf1550::Error::NotInitialized);
}

/** @brief The control output index of STANDBY in the port's GpioHooks. */
inline constexpr std::size_t kStandbyPin = 0U;
/** @brief The control input index of INTB in the port's GpioHooks. */
inline constexpr std::size_t kIntbPin = 0U;

/**
 * @brief The driver's I2C port over a kb::iface::I2cBus, with the STANDBY and INTB pins.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Port : public ::pf1550::I2cInterface<Port<Bus>>,
             public I2cHooks<Port<Bus>, Bus>,
             public GpioHooks<Port<Bus>, 1U, 1U> {
public:
  /** @brief Over `bus` with `timing` and the optional pins; no I/O. */
  Port(Bus& bus, const HookTiming& timing, const OutputPin& standby, const InputPin& intb) noexcept
      : I2cHooks<Port<Bus>, Bus>(bus, timing),
        GpioHooks<Port<Bus>, 1U, 1U>(std::array<OutputPin, 1U>{standby},
                                     std::array<InputPin, 1U>{intb}) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transfer (register address first). */
  bool Write(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: write the register address, then read with a repeated START. */
  bool WriteRead(uint8_t addr, const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                 std::size_t rx_len) noexcept {
    return this->BusWriteRead(addr, tx, tx_len, rx, rx_len);
  }
  /** @brief Hook: wait at least `us` (the DVS and charger waits). */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: drive STANDBY; false when the HAL supplied no pin. */
  bool GpioSet(::pf1550::CtrlPin pin, ::pf1550::GpioSignal level) noexcept {
    bool ok = false;
    if (pin == ::pf1550::CtrlPin::Standby) {
      ok = this->DriveOutput(kStandbyPin, level == ::pf1550::GpioSignal::ACTIVE);
    }
    return ok;
  }
  /** @brief Optional hook: read INTB (asserted = low with `active_low`); false when absent. */
  bool GpioGet(::pf1550::CtrlPin pin, bool& asserted) noexcept {
    bool ok = false;
    asserted = false;
    if (pin == ::pf1550::CtrlPin::Intb) {
      ok = this->ReadInput(kIntbPin, asserted);
    }
    return ok;
  }
  /** @brief Optional hook: why the last transfer failed. */
  bool GetLastFailure(::pf1550::I2cFailure& failure) noexcept {
    const kb::iface::I2cFailure f = this->BusLastFailure();
    switch (f) {
    case kb::iface::I2cFailure::None:
      failure = ::pf1550::I2cFailure::None;
      break;
    case kb::iface::I2cFailure::AddressNack:
      failure = ::pf1550::I2cFailure::AddressNack;
      break;
    case kb::iface::I2cFailure::DataNack:
      failure = ::pf1550::I2cFailure::DataNack;
      break;
    case kb::iface::I2cFailure::Bus:
    case kb::iface::I2cFailure::Timeout:
    case kb::iface::I2cFailure::BusStuck:
      failure = ::pf1550::I2cFailure::Bus;
      break;
    }
    return f != kb::iface::I2cFailure::None;
  }
  /** @brief The I2C transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return I2cHooks<Port<Bus>, Bus>::Counters();
  }
};

/** @brief The driver type a handler over `Bus` owns. */
template <kb::iface::I2cBus Bus>
using Driver = ::pf1550::PF1550<Port<Bus>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::pf1550::Config chip{};                   ///< Limits, acknowledgements and every block.
  HandlerConfig handler{};                   ///< Health, recovery, polling, sample period.
  kb::Milliseconds identify_deadline{50U};   ///< Bound of the Identify stage.
  kb::Milliseconds configure_deadline{100U}; ///< Bound of the Configure stage.
  uint8_t address{::pf1550::kI2cAddress};    ///< 7-bit address (0x08 to 0x0F, OTP).
  OutputPin standby{};                       ///< STANDBY input of the chip; absent: not wired.
  InputPin intb{};                           ///< INTB open-drain output (`active_low = true`).
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::pf1550::ChargerStatus charger{};                         ///< One pass over the charger status.
  ::pf1550::PowerState state{::pf1550::PowerState::Unknown}; ///< STATE_INFO.
  ::pf1550::InterruptEvents events{}; ///< Status bits latched by any call since the last service.
  bool intb_asserted{false}; ///< INTB low: an unmasked source is set (valid with `has_intb`).
  bool has_intb{false};      ///< The HAL supplied INTB.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SetChargerMode = 0,    ///< Charger to `mode` (charge only with limits-covered registers).
  RestartCharger = 1,    ///< Leave a latched charger fault state.
  ServiceWatchdog = 2,   ///< Clear the charger watchdog.
  ServiceInterrupts = 3, ///< Read and clear the interrupt status (the INTB service).
  ReadRail = 4,          ///< Read `rail`.
  SetRailVoltage = 5,    ///< `rail` to `microvolts` (LDOs, DVS bucks).
  SetRailModes = 6,      ///< Enables of `rail` in RUN, STANDBY, SLEEP to `modes`.
  RequestStandby = 7,    ///< STANDBY pin to `flag`.
  SelfCheck = 8,         ///< Identity, state, rails, faults and the input limit.
  CancelShutdown = 9,    ///< Cancel a pending ship or core-off request.
  SetIrqMask = 10,       ///< Interrupt masks to `mask`.
  ReadRegister = 11,     ///< One documented register at `address`.
};

/** @brief One command. */
struct Command {
  Op op{Op::SelfCheck};                                   ///< What to do.
  ::pf1550::Rail rail{::pf1550::Rail::Sw1};               ///< For the rail commands.
  ::pf1550::ChargerMode mode{::pf1550::ChargerMode::Off}; ///< For SetChargerMode.
  ::pf1550::RailModes modes{};                            ///< For SetRailModes.
  ::pf1550::IrqMask mask{};                               ///< For SetIrqMask.
  uint32_t microvolts{0U};                                ///< For SetRailVoltage.
  uint8_t address{0U};                                    ///< For ReadRegister.
  bool flag{false};                                       ///< For RequestStandby.
};

/** @brief The reply of a command. */
struct Reply {
  ::pf1550::InterruptEvents events{}; ///< ServiceInterrupts.
  ::pf1550::RailReadout rail{};       ///< ReadRail.
  ::pf1550::SelfCheckReport check{};  ///< SelfCheck.
  uint8_t value{0U};                  ///< ReadRegister.
};

/**
 * @brief The pf1550 handler.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Handler : public HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: identify and check the charger as found, then write the configuration.
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kIdentify, StageMode::Sync, false}, {stage::kConfigure, StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::I2c; ///< On an I2C bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing, config.standby, config.intb), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Bus>>& slot) noexcept {
    slot.emplace(port_, config_.chip, config_.address);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Bus>& driver) noexcept {
    const kb::Result<void> r = (call.index == 0U)
                                   ? LiftStatus(driver.EnsureInitialized(), &ToStatus)
                                   : LiftStatus(driver.Configure(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.identify_deadline : config_.configure_deadline;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::pf1550::Error::DeviceReset)) ||
           (detail == DetailOf(::pf1550::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Bus>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    Snapshot s{};
    kb::Result<void> r = kb::Result<void>::Success();
    const ::pf1550::Result<::pf1550::ChargerStatus> charger = driver.ReadChargerStatus();
    s.charger = charger.value;
    r = LiftStatus(charger, &ToStatus);
    if (r.IsOk()) {
      const ::pf1550::Result<::pf1550::PowerState> state = driver.ReadPowerState();
      s.state = state.value;
      r = LiftStatus(state, &ToStatus);
    }
    s.has_intb = port_.HasInput(kIntbPin);
    if (r.IsOk() && s.has_intb) {
      const ::pf1550::Result<bool> intb = driver.IntbAsserted();
      s.intb_asserted = intb.value;
      r = LiftStatus(intb, &ToStatus);
    }
    s.events = driver.GetLatchedEvents();
    return r.IsOk() ? kb::Result<Snapshot>::Success(s) : Forward<Snapshot>(r);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Bus>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    Reply reply{};
    kb::Result<void> r = kb::Result<void>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SetChargerMode:
      r = LiftStatus(driver.SetChargerMode(c.mode), &ToStatus);
      break;
    case Op::RestartCharger:
      r = LiftStatus(driver.RestartCharger(), &ToStatus);
      break;
    case Op::ServiceWatchdog:
      r = LiftStatus(driver.ServiceChargerWatchdog(), &ToStatus);
      break;
    case Op::ServiceInterrupts:
      r = take(driver.ServiceInterrupts(), reply.events);
      break;
    case Op::ReadRail:
      r = take(driver.ReadRail(c.rail), reply.rail);
      break;
    case Op::SetRailVoltage:
      r = LiftStatus(driver.SetRailVoltage(c.rail, ::pf1550::Microvolts{c.microvolts}), &ToStatus);
      break;
    case Op::SetRailModes:
      r = LiftStatus(driver.SetRailModes(c.rail, c.modes), &ToStatus);
      break;
    case Op::RequestStandby:
      r = LiftStatus(driver.RequestStandby(c.flag), &ToStatus);
      break;
    case Op::SelfCheck:
      r = take(driver.SelfCheck(), reply.check);
      break;
    case Op::CancelShutdown:
      r = LiftStatus(driver.CancelShutdownRequest(), &ToStatus);
      break;
    case Op::SetIrqMask:
      r = LiftStatus(driver.SetIrqMask(c.mask), &ToStatus);
      break;
    case Op::ReadRegister:
      r = take(driver.ReadRegister(c.address), reply.value);
      break;
    }
    return r.IsOk() ? kb::Result<Reply>::Success(reply) : Forward<Reply>(r);
  }
  /// @}

  /** @brief The port's I2C transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  // A driver result's value into `out` (also on a failure: the driver's `T{}`), as
  // kb::Result<void>.
  template <typename T>
  [[nodiscard]] static kb::Result<void> take(const ::pf1550::Result<T>& r, T& out) noexcept {
    out = r.value;
    return LiftStatus(r, &ToStatus);
  }

  Port<Bus> port_;
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

/** @brief The charger to `mode`; `Charge` only with limits-covered registers and no fault. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetChargerMode(Access& io, ::pf1550::ChargerMode mode,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetChargerMode;
  c.mode = mode;
  return Send(io, c, timeout);
}

/** @brief Leaves a latched charger fault state (off, verify, charge). */
template <typename Access>
[[nodiscard]] kb::Result<void> RestartCharger(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::RestartCharger;
  return Send(io, c, timeout);
}

/** @brief Clears the charger watchdog (call at least every `pf1550::kWatchdogServiceMaxS` when on).
 */
template <typename Access>
[[nodiscard]] kb::Result<void> ServiceChargerWatchdog(Access& io,
                                                      kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ServiceWatchdog;
  return Send(io, c, timeout);
}

/** @brief Reads and clears the interrupt status (the task an INTB interrupt notifies calls this).
 */
template <typename Access>
[[nodiscard]] kb::Result<::pf1550::InterruptEvents> ServiceInterrupts(
    Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ServiceInterrupts;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::pf1550::InterruptEvents>::Success(r.Value().events)
                  : Forward<::pf1550::InterruptEvents>(r);
}

/** @brief One rail as the chip holds it. */
template <typename Access>
[[nodiscard]] kb::Result<::pf1550::RailReadout> ReadRail(Access& io, ::pf1550::Rail rail,
                                                         kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadRail;
  c.rail = rail;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::pf1550::RailReadout>::Success(r.Value().rail)
                  : Forward<::pf1550::RailReadout>(r);
}

/** @brief `rail` to `uv` (an LDO or a DVS buck, inside its `Limits` window). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetRailVoltage(Access& io, ::pf1550::Rail rail,
                                              ::pf1550::Microvolts uv,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetRailVoltage;
  c.rail = rail;
  c.microvolts = uv.value;
  return Send(io, c, timeout);
}

/** @brief Drives STANDBY (`standby` true requests the STANDBY state). */
template <typename Access>
[[nodiscard]] kb::Result<void> RequestStandby(Access& io, bool standby,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::RequestStandby;
  c.flag = standby;
  return Send(io, c, timeout);
}

/** @brief Identity, state Run, rails as configured, no latched fault, the input limit. */
template <typename Access>
[[nodiscard]] kb::Result<::pf1550::SelfCheckReport> SelfCheck(Access& io,
                                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SelfCheck;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::pf1550::SelfCheckReport>::Success(r.Value().check)
                  : Forward<::pf1550::SelfCheckReport>(r);
}

/// @}

} // namespace kb::handlers::pf1550

#endif // KB_HANDLERS_BINDINGS_PF1550_HPP
