/**
 * @file pca9685.hpp
 * @brief The pca9685 binding and handler: PCA9685 16-channel, 12-bit I2C PWM / LED controller.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Bus, kOutputEnable>`: the driver's I2C hooks over a kb::iface::I2cBus (I2cHooks),
 * `GetLastFailure` (an absent chip is `NoDevice`), the oscillator delay, and with `kOutputEnable`
 * the OE pin (`GpioSet`, GpioHooks) | | Errors | `ToStatus()`: every `pca9685::Error` to a
 * kb::Status; the code in the health record's detail | | Start-up | `Identify + Configure` =
 * `driver.EnsureInitialized()` (sync): every output off, clock and prescaler, identity by
 * read-back, wake, OE asserted | | Thread model | Guarded (`kRequiresOwner = false`): set-points
 * written now and then from several tasks; on an owner bus the sample is a reset check | | Health |
 * `DeviceReset` (power-on or software reset seen) and `NotInitialized` fault at once; other
 * failures count to the threshold | | App API | snapshot `{uncertain, frequency_hz, may_be_asleep}`
 * after a reset check; `SetDuty()`, `SetPulseWidthUs()`, `SetChannelOff()`, `SetAllOff()`,
 * `SetPwmFrequency()`, `DisableOutputs()`, `EnableOutputs()`; the rest through `Command{Op::...}` |
 *
 * `kOutputEnable` says whether the board wires OE to the MCU. Without it the port has no `GpioSet`
 * hook, so `DisableOutputs()` and `EnableOutputs()` return `Unsupported` rather than claiming an
 * effect they cannot have. With it, `Config::output_enable` must name the pin (OE is active low:
 * asserted = outputs enabled = pin low).
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c1{bus};
 * kb::handlers::Guarded<kb::handlers::pca9685::Handler<Bus, true>, decltype(i2c1)> pwm{i2c1, bus,
 * timing, cfg}; if (pwm.EnsureStarted(kb::Milliseconds{20U}).IsOk()) {
 *   static_cast<void>(kb::handlers::pca9685::SetPulseWidthUs(pwm, 0U, 1500U,
 * kb::Milliseconds{5U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_PCA9685_HPP
#define KB_HANDLERS_BINDINGS_PCA9685_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <pca9685.hpp>

namespace kb::handlers::pca9685 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::pca9685::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::pca9685::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::pca9685::Error::NotInitialized:
  case ::pca9685::Error::DeviceReset:
  case ::pca9685::Error::Asleep:
    s = kb::Status::NotReady;
    break;
  case ::pca9685::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::pca9685::Error::Bus:
  case ::pca9685::Error::InvalidReply:
    s = kb::Status::BusError;
    break;
  case ::pca9685::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::pca9685::Error::DeviceIdMismatch:
  case ::pca9685::Error::DeviceFault:
  case ::pca9685::Error::WriteMismatch:
  case ::pca9685::Error::StateUnknown:
  case ::pca9685::Error::ExternalClockLatched:
    s = kb::Status::Fault;
    break;
  case ::pca9685::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::pca9685::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::pca9685::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::pca9685::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief The control output of the port: OE (asserted = outputs enabled; the pin is active low).
 */
inline constexpr std::size_t kOutputEnablePin = 0U;

/**
 * @brief The driver's I2C port over a kb::iface::I2cBus and, with `kOutputEnable`, the OE pin.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 * @tparam kOutputEnable True when the board wires OE to the MCU (the port then has `GpioSet`).
 */
template <kb::iface::I2cBus Bus, bool kOutputEnable>
class Port : public ::pca9685::I2cInterface<Port<Bus, kOutputEnable>>,
             public I2cHooks<Port<Bus, kOutputEnable>, Bus>,
             public GpioHooks<Port<Bus, kOutputEnable>, 1U, 0U> {
  using Wire = I2cHooks<Port<Bus, kOutputEnable>, Bus>;
  using Pins = GpioHooks<Port<Bus, kOutputEnable>, 1U, 0U>;

public:
  /** @brief Over `bus` with `timing` and the OE pin `oe`; no I/O. */
  Port(Bus& bus, const HookTiming& timing, const OutputPin& oe) noexcept
      : Wire(bus, timing), Pins(std::array<OutputPin, 1U>{oe}, std::array<InputPin, 0U>{}) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transfer. */
  bool Write(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: write then read with a repeated START. */
  bool WriteRead(uint8_t addr, const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                 std::size_t rx_len) noexcept {
    return this->BusWriteRead(addr, tx, tx_len, rx, rx_len);
  }
  /** @brief Hook: wait at least `us` microseconds (oscillator start-up). */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: OE (only with `kOutputEnable`); false when the pin is absent or failed.
   */
  bool GpioSet(::pca9685::CtrlPin pin, ::pca9685::GpioSignal signal) noexcept
    requires kOutputEnable
  {
    static_cast<void>(pin);
    return this->DriveOutput(kOutputEnablePin, signal == ::pca9685::GpioSignal::ACTIVE);
  }
  /** @brief Optional hook: why the last transfer failed. */
  bool GetLastFailure(::pca9685::I2cFailure& failure) noexcept {
    const kb::iface::I2cFailure f = this->BusLastFailure();
    switch (f) {
    case kb::iface::I2cFailure::AddressNack:
      failure = ::pca9685::I2cFailure::AddressNack;
      break;
    case kb::iface::I2cFailure::DataNack:
      failure = ::pca9685::I2cFailure::DataNack;
      break;
    case kb::iface::I2cFailure::None:
    case kb::iface::I2cFailure::Bus:
    case kb::iface::I2cFailure::Timeout:
    case kb::iface::I2cFailure::BusStuck:
      failure = ::pca9685::I2cFailure::Other;
      break;
    }
    return f != kb::iface::I2cFailure::None;
  }

  /** @brief The transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Wire::Counters();
  }
  /** @brief The pin counters. */
  [[nodiscard]] const HookCounters& PinCounters() const noexcept {
    return Pins::Counters();
  }
};

/** @brief The driver type a handler over `Bus` owns. */
template <kb::iface::I2cBus Bus, bool kOutputEnable = false>
using Driver = ::pca9685::PCA9685<Port<Bus, kOutputEnable>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::pca9685::Config chip{};                ///< PWM frequency, oscillator, output drive, inversion.
  HandlerConfig handler{};                 ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{20U};    ///< Bound of the start-up stage.
  uint8_t address{::pca9685::kI2cAddress}; ///< 7-bit address (0x40 to 0x7F, not 0x70).
  OutputPin output_enable{{}, true};       ///< OE, active low; used with `kOutputEnable`.
};

/** @brief What a sample publishes: the state after a reset check. */
struct Snapshot {
  uint32_t frequency_hz{0U}; ///< The PWM frequency the prescaler gives.
  uint16_t uncertain{0U};    ///< Channels whose last write failed, bit n = channel n.
  bool may_be_asleep{false}; ///< The chip is or may be asleep.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SetPwm = 0,          ///< Channel `channel` to `pwm`.
  SetDuty = 1,         ///< Channel `channel` to `duty` of 4096 (4096 = full on) at `phase`.
  SetPulseWidthUs = 2, ///< Channel `channel` to a pulse of `value` us (reply: the width set, ns).
  SetFullOn = 3,       ///< Channel `channel` full on.
  SetChannelOff = 4,   ///< Channel `channel` off.
  SetChannelsOff = 5,  ///< The channels in `mask` off.
  SetAllOff = 6,       ///< Every channel off (ALL_LED_OFF).
  SetPwmFrequency = 7, ///< The PWM frequency to `value` hertz.
  DisableOutputs = 8,  ///< OE high: every output off at once.
  EnableOutputs = 9,   ///< OE low.
  Sleep = 10,          ///< Sleep in `sleep` mode.
  Wake = 11,           ///< Wake.
  ReadChannel = 12,    ///< Channel `channel` read from the chip (reply: `pwm`).
  CheckForReset = 13,  ///< Check the chip still holds its configuration.
};

/** @brief One command. */
struct Command {
  ::pca9685::Pwm pwm{};                              ///< SetPwm.
  uint32_t value{0U};                                ///< Pulse width (us) or frequency (Hz).
  uint16_t mask{0U};                                 ///< SetChannelsOff: bit n = channel n.
  uint16_t duty{0U};                                 ///< SetDuty: 0 to 4096.
  uint16_t phase{0U};                                ///< SetDuty: on count, 0 to 4095.
  uint8_t channel{0U};                               ///< 0 to 15.
  Op op{Op::CheckForReset};                          ///< What to do.
  ::pca9685::Verify verify{::pca9685::Verify::None}; ///< Read back after a channel write.
  ::pca9685::SleepMode sleep{::pca9685::SleepMode::OutputsOff}; ///< Sleep.
};

/** @brief The reply of a command. */
struct Reply {
  ::pca9685::Pwm pwm{}; ///< ReadChannel: the channel.
  uint32_t value{0U}; ///< SetPulseWidthUs: the width set, nanoseconds (one count of quantisation).
};

/**
 * @brief The pca9685 handler.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 * @tparam kOutputEnable True when the board wires OE to the MCU.
 */
template <kb::iface::I2cBus Bus, bool kOutputEnable = false>
class Handler : public HandlerCore<Handler<Bus, kOutputEnable>, Driver<Bus, kOutputEnable>,
                                   Snapshot, Command, Reply> {
  using Drv = Driver<Bus, kOutputEnable>;
  using Core = HandlerCore<Handler<Bus, kOutputEnable>, Drv, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The chip has no reset pin; the driver drives OE itself.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::I2c; ///< On an I2C bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing, config.output_enable), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Drv>& slot) noexcept {
    slot.emplace(port_, config_.address, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call, Drv& driver) noexcept {
    static_cast<void>(call);
    const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Drv& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::pca9685::Error::DeviceReset)) ||
           (detail == DetailOf(::pca9685::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Drv& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<void> checked = LiftStatus(driver.CheckForReset(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(checked);
    if (checked.IsOk()) {
      Snapshot s{};
      s.frequency_hz = driver.GetPwmFrequencyHz().value;
      s.uncertain = driver.GetUncertainChannels();
      s.may_be_asleep = driver.MayBeAsleep();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Drv& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SetPwm:
      result = done(LiftStatus(driver.SetPwm(c.channel, c.pwm, c.verify), &ToStatus));
      break;
    case Op::SetDuty:
      result = done(LiftStatus(driver.SetDuty(c.channel, c.duty, c.phase, c.verify), &ToStatus));
      break;
    case Op::SetPulseWidthUs:
      result = width(Lift(driver.SetPulseWidthUs(c.channel, c.value, c.verify), &ToStatus));
      break;
    case Op::SetFullOn:
      result = done(LiftStatus(driver.SetFullOn(c.channel, c.verify), &ToStatus));
      break;
    case Op::SetChannelOff:
      result = done(LiftStatus(driver.SetChannelOff(c.channel, c.verify), &ToStatus));
      break;
    case Op::SetChannelsOff:
      result = done(LiftStatus(driver.SetChannelsOff(c.mask), &ToStatus));
      break;
    case Op::SetAllOff:
      result = done(LiftStatus(driver.SetAllOff(), &ToStatus));
      break;
    case Op::SetPwmFrequency:
      result = done(LiftStatus(driver.SetPwmFrequency(c.value), &ToStatus));
      break;
    case Op::DisableOutputs:
      result = done(LiftStatus(driver.DisableOutputs(), &ToStatus));
      break;
    case Op::EnableOutputs:
      result = done(LiftStatus(driver.EnableOutputs(), &ToStatus));
      break;
    case Op::Sleep:
      result = done(LiftStatus(driver.Sleep(c.sleep), &ToStatus));
      break;
    case Op::Wake:
      result = done(LiftStatus(driver.Wake(), &ToStatus));
      break;
    case Op::ReadChannel:
      result = channel(Lift(driver.ReadChannel(c.channel), &ToStatus));
      break;
    case Op::CheckForReset:
      result = done(LiftStatus(driver.CheckForReset(), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> width(const kb::Result<uint32_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> channel(const kb::Result<::pca9685::Pwm>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.pwm = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Bus, kOutputEnable> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Runs `c` and drops its reply value. */
template <typename Access>
[[nodiscard]] kb::Result<void> RunCommand(Access& io, const Command& c,
                                          kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Channel `ch` to `duty` of 4096 (0 = off, 4096 = full on), starting at `phase`. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetDuty(Access& io, uint8_t ch, uint16_t duty,
                                       kb::Milliseconds timeout, uint16_t phase = 0U) noexcept {
  Command c{};
  c.op = Op::SetDuty;
  c.channel = ch;
  c.duty = duty;
  c.phase = phase;
  return RunCommand(io, c, timeout);
}

/** @brief Channel `ch` to a pulse of `width_us` microseconds; the width set, in nanoseconds. */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> SetPulseWidthUs(Access& io, uint8_t ch, uint32_t width_us,
                                                   kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetPulseWidthUs;
  c.channel = ch;
  c.value = width_us;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().value) : Forward<uint32_t>(r);
}

/** @brief Channel `ch` off. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetChannelOff(Access& io, uint8_t ch,
                                             kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetChannelOff;
  c.channel = ch;
  return RunCommand(io, c, timeout);
}

/** @brief Every channel off. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetAllOff(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetAllOff;
  return RunCommand(io, c, timeout);
}

/** @brief The PWM frequency to `hz` (the chip sleeps for the prescaler change). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetPwmFrequency(Access& io, uint32_t hz,
                                               kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetPwmFrequency;
  c.value = hz;
  return RunCommand(io, c, timeout);
}

/** @brief OE high: every output off at once (needs `kOutputEnable`). */
template <typename Access>
[[nodiscard]] kb::Result<void> DisableOutputs(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisableOutputs;
  return RunCommand(io, c, timeout);
}

/** @brief OE low: the outputs follow the channels (needs `kOutputEnable`). */
template <typename Access>
[[nodiscard]] kb::Result<void> EnableOutputs(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::EnableOutputs;
  return RunCommand(io, c, timeout);
}

/// @}

} // namespace kb::handlers::pca9685

#endif // KB_HANDLERS_BINDINGS_PCA9685_HPP
