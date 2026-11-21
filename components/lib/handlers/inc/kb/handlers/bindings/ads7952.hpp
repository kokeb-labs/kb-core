/**
 * @file ads7952.hpp
 * @brief The ads7952 binding and handler: ADS7950 / ADS7951 / ADS7952 / ADS7953 4- to 16-channel,
 * 12-bit SPI SAR ADC (Manual, Auto-1 and Auto-2 sequencing, alarms, GPIO).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks, one 16-bit
 * frame per transfer) and the power-up delay | | Errors | `ToStatus()`: every `ads7952::Error` to a
 * kb::Status; the code in the health record's detail | | Start-up | `Identify + Configure` =
 * `driver.EnsureInitialized()` (sync): register reset, every program register written,
 * channel-address identity check, optional scale monitor | | Thread model | Either (`kRequiresOwner
 * = false`): on an owner bus the owner task scans Auto-1 every `sample_period` and publishes it
 * (the usual choice for control loops); on a guarded bus a task reads channels on demand | | Health
 * | `DeviceReset` (a brown-out seen) and `NotInitialized` fault at once; lost frames, sequence and
 * range mismatches and bus failures count to the threshold | | App API | snapshot = one Auto-1 scan
 * `{code[16], mask, range}`; `ReadChannel()`, `ScanAuto1()`, `SetRange()`, `SetGpioOutputs()`,
 * `ReadGpio()`, `CheckScale()`; the rest through `Command{Op::...}` |
 *
 * Codes convert to microvolts with `ads7952::CodeToMicrovolts(code, range, vref_mv)`.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Owner> spi1{adc_cs};
 * kb::handlers::OwnerAccess<kb::handlers::ads7952::Handler<Dev>, decltype(spi1)> adc{spi1, adc_cs,
 * timing, cfg};
 * // The owner task calls spi1.Service(now, adc) every step; any task then reads:
 * const auto scan = adc.Latest();  // the newest Auto-1 scan, its sequence and age
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_ADS7952_HPP
#define KB_HANDLERS_BINDINGS_ADS7952_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <ads7952.hpp>

namespace kb::handlers::ads7952 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::ads7952::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::ads7952::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::ads7952::Error::NotInitialized:
  case ::ads7952::Error::DeviceReset:
    s = kb::Status::NotReady;
    break;
  case ::ads7952::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::ads7952::Error::Bus:
  case ::ads7952::Error::ChannelMismatch:
  case ::ads7952::Error::SequenceMismatch:
    s = kb::Status::BusError;
    break;
  case ::ads7952::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::ads7952::Error::DeviceIdMismatch:
  case ::ads7952::Error::DeviceFault:
  case ::ads7952::Error::WriteMismatch:
  case ::ads7952::Error::RangeMismatch:
  case ::ads7952::Error::ScaleMismatch:
    s = kb::Status::Fault;
    break;
  case ::ads7952::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::ads7952::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::ads7952::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::ads7952::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/**
 * @brief The driver's SPI port over a kb::iface::SpiDevice.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::ads7952::SpiInterface<Port<Dev>>, public SpiHooks<Port<Dev>, Dev> {
public:
  /** @brief Over `dev` with `timing`; no I/O. */
  Port(Dev& dev, const HookTiming& timing) noexcept : SpiHooks<Port<Dev>, Dev>(dev, timing) {}

  /** @brief Hook: bring the device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one 16-bit frame with the chip select held. */
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return this->BusTransfer(tx, rx, len);
  }
  /** @brief Hook: wait at least `us` microseconds (power-up after the reset). */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::ads7952::ADS7952<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::ads7952::Config chip{};             ///< Variant, package, range, VREF, sequences, GPIO, alarms.
  HandlerConfig handler{};              ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{20U}; ///< Bound of the start-up stage.
};

/** @brief One scan: what a sample publishes and what the scan commands return. */
struct Snapshot {
  std::array<uint16_t, ::ads7952::kMaxChannels> code{}; ///< Result of channel n at index n.
  uint16_t mask{0U};                                    ///< The channels of the scan.
  ::ads7952::Range range{::ads7952::Range::Vref};       ///< The range of every code.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadChannel = 0,         ///< Channel `channel` in Manual mode (reply: `value`).
  ScanAuto1 = 1,           ///< One pass of the Auto-1 sequence (reply: `scan`).
  ScanAuto2 = 2,           ///< One pass of the Auto-2 sequence (reply: `scan`).
  SetRange = 3,            ///< The input range to `range`.
  SetAuto1Channels = 4,    ///< The Auto-1 sequence to `mask`.
  SetAuto2LastChannel = 5, ///< The last channel of Auto-2 to `channel`.
  SetAlarmThresholds = 6,  ///< The thresholds of `channel` to `thresholds`.
  SetGpioOutputs = 7,      ///< The GPIO output levels to `value`.
  ReadGpio = 8,            ///< The GPIO pin levels (reply: `value`).
  CheckScale = 9,          ///< The scale monitor (reply: its code in `value`).
};

/** @brief One command. */
struct Command {
  ::ads7952::AlarmThreshold thresholds{};            ///< SetAlarmThresholds.
  uint16_t mask{0U};                                 ///< SetAuto1Channels.
  uint8_t channel{0U};                               ///< ReadChannel, SetAuto2LastChannel, alarms.
  uint8_t value{0U};                                 ///< SetGpioOutputs: bit n for GPIOn.
  Op op{Op::ReadChannel};                            ///< What to do.
  ::ads7952::Verify verify{::ads7952::Verify::None}; ///< Read back sequences and GPIO levels.
  ::ads7952::Range range{::ads7952::Range::Vref};    ///< SetRange.
};

/** @brief The reply of a command. */
struct Reply {
  Snapshot scan{};    ///< ScanAuto1, ScanAuto2.
  uint16_t value{0U}; ///< ReadChannel and CheckScale: the code; ReadGpio: the levels.
};

/**
 * @brief The ads7952 handler.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The chip has no reset or enable pin; the driver resets its registers by command.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Either model; an owner bus samples it.
  static constexpr BusKind kBus = BusKind::Spi; ///< On an SPI chip select.

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
    return (detail == DetailOf(::ads7952::Error::DeviceReset)) ||
           (detail == DetailOf(::ads7952::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    return ToSnapshot(Lift(driver.ReadAuto1(), &ToStatus));
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadChannel:
      result = value(Lift(driver.ReadChannel(c.channel), &ToStatus));
      break;
    case Op::ScanAuto1:
      result = scan(ToSnapshot(Lift(driver.ReadAuto1(), &ToStatus)));
      break;
    case Op::ScanAuto2:
      result = scan(ToSnapshot(Lift(driver.ReadAuto2(), &ToStatus)));
      break;
    case Op::SetRange:
      result = done(LiftStatus(driver.SetRange(c.range), &ToStatus));
      break;
    case Op::SetAuto1Channels:
      result = done(LiftStatus(driver.SetAuto1Channels(c.mask, c.verify), &ToStatus));
      break;
    case Op::SetAuto2LastChannel:
      result = done(LiftStatus(driver.SetAuto2LastChannel(c.channel, c.verify), &ToStatus));
      break;
    case Op::SetAlarmThresholds:
      result = done(LiftStatus(driver.SetAlarmThresholds(c.channel, c.thresholds), &ToStatus));
      break;
    case Op::SetGpioOutputs:
      result = done(LiftStatus(driver.SetGpioOutputs(c.value, c.verify), &ToStatus));
      break;
    case Op::ReadGpio:
      result = value(Lift(driver.ReadGpio(), &ToStatus));
      break;
    case Op::CheckScale:
      result = value(Lift(driver.CheckScale(), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.Counters();
  }

  /** @brief A driver bank as a snapshot. */
  [[nodiscard]] static kb::Result<Snapshot> ToSnapshot(
      const kb::Result<::ads7952::Bank>& r) noexcept {
    kb::Result<Snapshot> result = Forward<Snapshot>(r);
    if (r.IsOk()) {
      Snapshot s{};
      s.code = r.Value().code;
      s.mask = r.Value().mask;
      s.range = r.Value().range;
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }

private:
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> scan(const kb::Result<Snapshot>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.scan = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  template <typename T>
  [[nodiscard]] static kb::Result<Reply> value(const kb::Result<T>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = static_cast<uint16_t>(r.Value());
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Dev> port_;
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

/** @brief The code of channel `ch`, converted now (Manual mode). */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> ReadChannel(Access& io, uint8_t ch,
                                               kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadChannel;
  c.channel = ch;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/** @brief One Auto-1 pass now (on an owner bus, `Latest()` returns the periodic one). */
template <typename Access>
[[nodiscard]] kb::Result<Snapshot> ScanAuto1(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ScanAuto1;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<Snapshot>::Success(r.Value().scan) : Forward<Snapshot>(r);
}

/** @brief The input range to `range`. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetRange(Access& io, ::ads7952::Range range,
                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetRange;
  c.range = range;
  return RunCommand(io, c, timeout);
}

/** @brief The GPIO output levels (bit n for GPIOn, output pins only). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetGpioOutputs(
    Access& io, uint8_t levels, kb::Milliseconds timeout,
    ::ads7952::Verify verify = ::ads7952::Verify::None) noexcept {
  Command c{};
  c.op = Op::SetGpioOutputs;
  c.value = levels;
  c.verify = verify;
  return RunCommand(io, c, timeout);
}

/** @brief The GPIO pin levels. */
template <typename Access>
[[nodiscard]] kb::Result<uint8_t> ReadGpio(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadGpio;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint8_t>::Success(static_cast<uint8_t>(r.Value().value))
                  : Forward<uint8_t>(r);
}

/** @brief The scale monitor's code, checked against its window. */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> CheckScale(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::CheckScale;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/// @}

} // namespace kb::handlers::ads7952

#endif // KB_HANDLERS_BINDINGS_ADS7952_HPP
