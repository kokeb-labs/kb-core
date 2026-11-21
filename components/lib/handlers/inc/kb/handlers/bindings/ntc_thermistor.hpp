/**
 * @file ntc_thermistor.hpp
 * @brief The ntc_thermistor binding and handler: an NTC thermistor in a resistor divider, read as
 * raw codes through one ADC channel (beta, Steinhart-Hart or table curve).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Adc>`: the driver's ADC hooks over a kb::iface::AdcChannel (AdcHooks, raw codes),
 * the delay, and the optional bias pin (`SetBias`, GpioHooks; no pin = the divider is always
 * powered) | | Errors | `ToStatus()`: every `ntc_thermistor::Error` to a kb::Status; the code in
 * the health record's detail | | Start-up | `Identify + Configure` = `driver.EnsureInitialized()`
 * (sync): configuration check, bias off; no conversion | | Thread model | Guarded (`kRequiresOwner
 * = false`): a slow sensor that needs no servicing; an owner bus samples it every `sample_period` |
 * | Health | `NotInitialized` faults at once; open and short circuits, out-of-range and unstable
 * readings and ADC failures count to the threshold | | App API | snapshot `{temperature_mc,
 * resistance_ohm, code, uncertainty_mc}`; `ReadTemperature()` (a fresh sample), `ReadResistance()`,
 * `ReadRawCode()` |
 *
 * The kb ADC channel is the thermistor's channel: `Config::chip.adc_channel` is passed to the hook
 * and not used by it. `Config::chip.adc_bits` must match the channel's resolution.
 *
 * @code
 * kb::handlers::BusDomain<Adc, kb::handlers::ThreadModel::Guarded> adc1{adc};
 * kb::handlers::Guarded<kb::handlers::ntc_thermistor::Handler<Adc>, decltype(adc1)> t{adc1, adc,
 * timing, cfg}; if (t.EnsureStarted(kb::Milliseconds{10U}).IsOk()) { const kb::Result<int32_t> mc =
 * kb::handlers::ntc_thermistor::ReadTemperature(t, kb::Milliseconds{10U});
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_NTC_THERMISTOR_HPP
#define KB_HANDLERS_BINDINGS_NTC_THERMISTOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <ntc_thermistor.hpp>

namespace kb::handlers::ntc_thermistor {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::ntc_thermistor::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::ntc_thermistor::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::ntc_thermistor::Error::NotInitialized:
    s = kb::Status::NotReady;
    break;
  case ::ntc_thermistor::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::ntc_thermistor::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::ntc_thermistor::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::ntc_thermistor::Error::DeviceIdMismatch:
  case ::ntc_thermistor::Error::DeviceFault:
  case ::ntc_thermistor::Error::ShortCircuit:
  case ::ntc_thermistor::Error::Unstable:
    s = kb::Status::Fault;
    break;
  case ::ntc_thermistor::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::ntc_thermistor::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::ntc_thermistor::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::ntc_thermistor::Error::OpenCircuit:
    s = kb::Status::NoDevice;
    break;
  case ::ntc_thermistor::Error::BelowRange:
  case ::ntc_thermistor::Error::AboveRange:
  case ::ntc_thermistor::Error::CodeOutOfScale:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief The control output of the port: the divider's bias (excitation) pin. */
inline constexpr std::size_t kBiasPin = 0U;

/**
 * @brief The driver's ADC port over a kb::iface::AdcChannel and the optional bias pin.
 * @tparam Adc The channel (an AdcChannel or AdcChannelRef).
 */
template <kb::iface::AdcChannel Adc>
class Port : public ::ntc_thermistor::AdcInterface<Port<Adc>>,
             public AdcHooks<Port<Adc>, Adc>,
             public GpioHooks<Port<Adc>, 1U, 0U> {
  using Analog = AdcHooks<Port<Adc>, Adc>;
  using Pins = GpioHooks<Port<Adc>, 1U, 0U>;

public:
  /** @brief Over `adc` with `timing` and the bias pin `bias` (absent: always powered); no I/O. */
  Port(Adc& adc, const HookTiming& timing, const OutputPin& bias) noexcept
      : Analog(adc, timing), Pins(std::array<OutputPin, 1U>{bias}, std::array<InputPin, 0U>{}) {}

  /** @brief Hook: the HAL brings the ADC up before the handler starts. */
  bool EnsureInitialized() noexcept {
    return true;
  }
  /** @brief Hook: one raw conversion. */
  bool ReadRaw(uint8_t channel, uint32_t& code) noexcept {
    static_cast<void>(channel);
    return this->AdcRaw(code);
  }
  /** @brief Optional hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: the bias pin; true without a pin (the divider is always powered). */
  bool SetBias(bool on) noexcept {
    bool ok = true;
    if (this->HasOutput(kBiasPin)) {
      ok = this->DriveOutput(kBiasPin, on);
    }
    return ok;
  }

  /** @brief The conversion counters. */
  [[nodiscard]] const HookCounters& AdcCounters() const noexcept {
    return Analog::Counters();
  }
  /** @brief The pin counters. */
  [[nodiscard]] const HookCounters& PinCounters() const noexcept {
    return Pins::Counters();
  }
};

/** @brief The driver type a handler over `Adc` owns. */
template <kb::iface::AdcChannel Adc>
using Driver = ::ntc_thermistor::NtcThermistor<Port<Adc>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::ntc_thermistor::Config
      chip{}; ///< Curve, divider, ADC bits, averaging (the table must outlive the handler).
  HandlerConfig handler{};              ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{10U}; ///< Bound of the start-up stage.
  OutputPin bias{};                     ///< The divider's bias pin; absent when always powered.
};

/** @brief What a sample publishes (one averaged reading). */
struct Snapshot {
  int32_t temperature_mc{0};   ///< Milli-degrees Celsius, window and offset applied.
  uint32_t resistance_ohm{0U}; ///< The thermistor's resistance.
  uint32_t code{0U};           ///< The mean ADC code.
  uint32_t uncertainty_mc{0U}; ///< Bound on the declared tolerances and one LSB.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadResistance = 0, ///< The thermistor's resistance now (averaged), ohms.
  ReadRawCode = 1,    ///< One conversion, the raw code.
};

/** @brief One command. */
struct Command {
  Op op{Op::ReadResistance}; ///< What to do.
};

/** @brief The reply of a command. */
struct Reply {
  uint32_t value{0U}; ///< Ohms or the code.
};

/**
 * @brief The ntc_thermistor handler.
 * @tparam Adc The channel (an AdcChannel or AdcChannelRef).
 */
template <kb::iface::AdcChannel Adc>
class Handler : public HandlerCore<Handler<Adc>, Driver<Adc>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Adc>, Driver<Adc>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The driver drives the bias pin itself, so there is no Power stage.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::Adc; ///< On an ADC channel.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Adc& adc, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(adc, timing, config.bias), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Adc>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Adc>& driver) noexcept {
    static_cast<void>(call);
    const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::ntc_thermistor::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Adc>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<::ntc_thermistor::Reading> r = Lift(driver.Read(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(r);
    if (r.IsOk()) {
      Snapshot s{};
      s.temperature_mc = r.Value().temperature_mc;
      s.resistance_ohm = r.Value().resistance_ohm;
      s.code = r.Value().code;
      s.uncertainty_mc = r.Value().uncertainty_mc;
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Adc>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadResistance:
      result = value(Lift(driver.ReadResistance(), &ToStatus));
      break;
    case Op::ReadRawCode:
      result = value(Lift(driver.ReadRawCode(), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's conversion counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.AdcCounters();
  }

private:
  [[nodiscard]] static kb::Result<Reply> value(const kb::Result<uint32_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Adc> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief A fresh reading (a sample, also published), temperature in milli-C. */
template <typename Access>
[[nodiscard]] kb::Result<int32_t> ReadTemperature(Access& io, kb::Milliseconds timeout) noexcept {
  const auto r = io.Sample(timeout);
  return r.IsOk() ? kb::Result<int32_t>::Success(r.Value().value.temperature_mc)
                  : Forward<int32_t>(r);
}

/** @brief The thermistor's resistance now, ohms. */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> ReadResistance(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadResistance;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().value) : Forward<uint32_t>(r);
}

/** @brief One conversion, the raw code (bring-up and wiring checks). */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> ReadRawCode(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadRawCode;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().value) : Forward<uint32_t>(r);
}

/// @}

} // namespace kb::handlers::ntc_thermistor

#endif // KB_HANDLERS_BINDINGS_NTC_THERMISTOR_HPP
