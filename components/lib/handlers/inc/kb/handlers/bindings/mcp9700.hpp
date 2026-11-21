/**
 * @file mcp9700.hpp
 * @brief The mcp9700 binding and handler: MCP9700 / MCP9700A / MCP9700B / MCP9701 / MCP9701A
 * linear active thermistor (analog output) read through one ADC channel.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Adc>`: the driver's ADC hooks over a kb::iface::AdcChannel (AdcHooks, calibrated
 * millivolts), the delay, and the optional supply pin (`GpioSet`, GpioHooks; no pin = the sensor
 * sits on the rail) | | Errors | `ToStatus()`: every `mcp9700::Error` to a kb::Status; the code in
 * the health record's detail | | Start-up | `Identify + Configure` = `driver.EnsureInitialized()`
 * (sync): configuration check, supply on, settle, first reading checked for a plausible, stable
 * output | | Thread model | Guarded (`kRequiresOwner = false`): a slow sensor that needs no
 * servicing; an owner bus samples it every `sample_period` | | Health | `NotInitialized` faults at
 * once; ADC failures and implausible readings count to the threshold | | App API | snapshot
 * `{temperature_mc, output_mv, spread_mv}`; `ReadTemperature()` (a fresh sample), `ReadRawMv()`
 * (one conversion) |
 *
 * The kb ADC channel is the sensor's channel: `Config::chip.channel` is passed to the hook and not
 * used by it.
 *
 * @code
 * kb::handlers::BusDomain<Adc, kb::handlers::ThreadModel::Guarded> adc0{adc};
 * kb::handlers::Guarded<kb::handlers::mcp9700::Handler<Adc>, decltype(adc0)> t{adc0, adc, timing,
 * cfg}; if (t.EnsureStarted(kb::Milliseconds{50U}).IsOk()) { const kb::Result<int32_t> mc =
 * kb::handlers::mcp9700::ReadTemperature(t, kb::Milliseconds{20U});
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_MCP9700_HPP
#define KB_HANDLERS_BINDINGS_MCP9700_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <mcp9700.hpp>

namespace kb::handlers::mcp9700 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::mcp9700::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::mcp9700::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::mcp9700::Error::NotInitialized:
    s = kb::Status::NotReady;
    break;
  case ::mcp9700::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::mcp9700::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::mcp9700::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::mcp9700::Error::DeviceIdMismatch:
  case ::mcp9700::Error::DeviceFault:
  case ::mcp9700::Error::Unstable:
    s = kb::Status::Fault;
    break;
  case ::mcp9700::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::mcp9700::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::mcp9700::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::mcp9700::Error::OutputLow:
  case ::mcp9700::Error::OutputHigh:
  case ::mcp9700::Error::Saturated:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief The control output of the port: the sensor's supply pin. */
inline constexpr std::size_t kEnablePin = 0U;

/**
 * @brief The driver's ADC port over a kb::iface::AdcChannel and the optional supply pin.
 * @tparam Adc The channel (an AdcChannel or AdcChannelRef).
 */
template <kb::iface::AdcChannel Adc>
class Port : public ::mcp9700::AdcInterface<Port<Adc>>,
             public AdcHooks<Port<Adc>, Adc>,
             public GpioHooks<Port<Adc>, 1U, 0U> {
  using Analog = AdcHooks<Port<Adc>, Adc>;
  using Pins = GpioHooks<Port<Adc>, 1U, 0U>;

public:
  /** @brief Over `adc` with `timing` and the supply pin `enable` (absent: on the rail); no I/O. */
  Port(Adc& adc, const HookTiming& timing, const OutputPin& enable) noexcept
      : Analog(adc, timing), Pins(std::array<OutputPin, 1U>{enable}, std::array<InputPin, 0U>{}) {}

  /** @brief Hook: the HAL brings the ADC up before the handler starts. */
  bool EnsureInitialized() noexcept {
    return true;
  }
  /** @brief Hook: one calibrated conversion in millivolts (a negative reading reads 0). */
  bool ReadVoltageMv(uint8_t channel, uint32_t& voltage_mv) noexcept {
    static_cast<void>(channel);
    int32_t mv = 0;
    const bool ok = this->AdcMillivolts(mv);
    voltage_mv = (mv > 0) ? static_cast<uint32_t>(mv) : 0U;
    return ok;
  }
  /** @brief Hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: the supply pin; true without a pin (the sensor is always powered). */
  bool GpioSet(::mcp9700::CtrlPin pin, ::mcp9700::GpioSignal signal) noexcept {
    static_cast<void>(pin);
    bool ok = true;
    if (this->HasOutput(kEnablePin)) {
      ok = this->DriveOutput(kEnablePin, signal == ::mcp9700::GpioSignal::ACTIVE);
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
using Driver = ::mcp9700::MCP9700<Port<Adc>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::mcp9700::Config chip{};             ///< Variant, ADC scale, averaging, settle time.
  HandlerConfig handler{};              ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{50U}; ///< Bound of the start-up stage.
  OutputPin enable{};                   ///< The sensor's supply pin; absent when on the rail.
};

/** @brief What a sample publishes (one averaged, validated reading). */
struct Snapshot {
  int32_t temperature_mc{0}; ///< Temperature after the calibration offset, milli-C.
  uint32_t output_mv{0U};    ///< Mean sensor output, millivolts.
  uint32_t spread_mv{0U};    ///< Largest minus smallest conversion of the reading, millivolts.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadMeasurement = 0, ///< One averaged, validated reading now.
  ReadRawMv = 1,       ///< One conversion at the ADC pin, no check.
};

/** @brief One command. */
struct Command {
  Op op{Op::ReadMeasurement}; ///< What to do.
};

/** @brief The reply of a command. */
struct Reply {
  Snapshot reading{};  ///< ReadMeasurement: the reading.
  uint32_t raw_mv{0U}; ///< ReadRawMv: the conversion, millivolts.
};

/**
 * @brief The mcp9700 handler.
 * @tparam Adc The channel (an AdcChannel or AdcChannelRef).
 */
template <kb::iface::AdcChannel Adc>
class Handler : public HandlerCore<Handler<Adc>, Driver<Adc>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Adc>, Driver<Adc>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The driver drives the supply pin itself, so there is no Power stage.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::Adc; ///< On an ADC channel.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Adc& adc, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(adc, timing, config.enable), config_(config) {}

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
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Adc>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::mcp9700::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Adc>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    return reading(Lift(driver.ReadMeasurement(), &ToStatus));
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Adc>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadMeasurement: {
      const kb::Result<Snapshot> r = reading(Lift(driver.ReadMeasurement(), &ToStatus));
      result = Forward<Reply>(r);
      if (r.IsOk()) {
        Reply reply{};
        reply.reading = r.Value();
        result = kb::Result<Reply>::Success(reply);
      }
      break;
    }
    case Op::ReadRawMv: {
      const kb::Result<uint32_t> r = Lift(driver.ReadRawPortMv(), &ToStatus);
      result = Forward<Reply>(r);
      if (r.IsOk()) {
        Reply reply{};
        reply.raw_mv = r.Value();
        result = kb::Result<Reply>::Success(reply);
      }
      break;
    }
    }
    return result;
  }
  /// @}

  /** @brief The port's conversion counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.AdcCounters();
  }

private:
  [[nodiscard]] static kb::Result<Snapshot> reading(
      const kb::Result<::mcp9700::Measurement>& r) noexcept {
    kb::Result<Snapshot> result = Forward<Snapshot>(r);
    if (r.IsOk()) {
      Snapshot s{};
      s.temperature_mc = r.Value().temperature_mc;
      s.output_mv = r.Value().output_mv;
      s.spread_mv = r.Value().spread_mv;
      result = kb::Result<Snapshot>::Success(s);
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

/** @brief One conversion at the ADC pin, millivolts (bring-up and wiring checks). */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> ReadRawMv(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadRawMv;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().raw_mv) : Forward<uint32_t>(r);
}

/// @}

} // namespace kb::handlers::mcp9700

#endif // KB_HANDLERS_BINDINGS_MCP9700_HPP
