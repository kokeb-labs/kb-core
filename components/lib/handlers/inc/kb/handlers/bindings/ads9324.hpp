// clang-format off: the table below must keep one row per line.
/**
 * @file ads9324.hpp
 * @brief The ads9324 binding and handler: ADS9324 16-channel simultaneous-sampling SPI ADC.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks) and its control pins (GpioHooks: RESET and CONVST out, DRDY/ALARM in), with `DelayUs` through the hook timing (a spin below 1 ms, a sleep from 1 ms). `CaptureLanes` has no kb contract: data comes on SDOUT, one lane |
 * | Errors | `ToStatus()`: every `ads9324::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Identify + Configure` = `driver.EnsureInitialized()` (sync): power-up wait, reset (the RESET pin when `chip.hardware_reset`, else SW_RST), DEVICE_ID, configuration, optional calibration |
 * | Thread model | Owner task recommended (`kRequiresOwner = false`): a high-rate converter sampled at the control period and read lock-free with `Latest()`; a guarded bus works for occasional reads |
 * | Health | `UnexpectedReset` (RESET_DETECT_FLAG: the registers were lost) and `NotInitialized` fault at once; other failures count to the threshold |
 * | App API | snapshot `{codes, microvolts, present, clipped, window alert}` per frame; commands: window flags, calibration, power down and wake, reset, identify, alarm pin, data-path self-test, raw register |
 *
 * While the chip is powered down (`PowerDown`), `Measure()` sends nothing and republishes the last
 * frame with `standby` set, so a sampling owner task does not count standby as failures.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Owner> spi2{dev};
 * kb::handlers::OwnerAccess<kb::handlers::ads9324::Handler<Dev>, decltype(spi2)> adc{spi2, dev, timing, cfg};
 * // the owner task calls spi2.Service(now, adc) every control step
 * const auto frame = adc.Latest();  // frame.Value().value.microvolts[0]
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_ADS9324_HPP
#define KB_HANDLERS_BINDINGS_ADS9324_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <ads9324.hpp>

namespace kb::handlers::ads9324 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::ads9324::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::ads9324::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::ads9324::Error::NotInitialized:
  case ::ads9324::Error::UnexpectedReset:
  case ::ads9324::Error::Refused:
    s = kb::Status::NotReady;
    break;
  case ::ads9324::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::ads9324::Error::Bus:
  case ::ads9324::Error::DataMismatch:
    s = kb::Status::BusError;
    break;
  case ::ads9324::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::ads9324::Error::DeviceIdMismatch:
  case ::ads9324::Error::DeviceFault:
  case ::ads9324::Error::VerifyFailed:
    s = kb::Status::Fault;
    break;
  case ::ads9324::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::ads9324::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::ads9324::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::ads9324::Error error) noexcept {
  return (error == ::ads9324::Error::UnexpectedReset) ||
         (error == ::ads9324::Error::NotInitialized);
}

/// @name Control pin indices of the HAL's pin arrays
/// @{
inline constexpr std::size_t kPinReset = 0U;  ///< Output: RESET (wire it active low).
inline constexpr std::size_t kPinConvst = 1U; ///< Output: CONVST (active high).
inline constexpr std::size_t kPinAlarm = 0U;  ///< Input: DRDY/ALARM, its electrical level
                                              ///< (leave `active_low` false: the driver applies
                                              ///< `Alarm::active_low` itself).
/// @}

/**
 * @brief The driver's SPI port and control pins over kb contracts.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef): mode 0.
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::ads9324::SpiInterface<Port<Dev>>,
             public SpiHooks<Port<Dev>, Dev>,
             public GpioHooks<Port<Dev>, 2U, 1U> {
public:
  /** @brief Over `dev`, `timing` and the HAL's pins; no I/O. */
  Port(Dev& dev, const HookTiming& timing, const std::array<OutputPin, 2U>& outputs,
       const std::array<InputPin, 1U>& inputs) noexcept
      : SpiHooks<Port<Dev>, Dev>(dev, timing), GpioHooks<Port<Dev>, 2U, 1U>(outputs, inputs) {}

  /** @brief Hook: bring the device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one full-duplex transfer (`rx` may be null). */
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
  /** @brief Optional hook: drive RESET or CONVST; false for an absent pin or DRDY/ALARM. */
  bool GpioSet(::ads9324::CtrlPin pin, ::ads9324::GpioSignal level) noexcept {
    const bool active = level == ::ads9324::GpioSignal::ACTIVE;
    bool ok = false;
    switch (pin) {
    case ::ads9324::CtrlPin::Reset:
      ok = this->DriveOutput(kPinReset, active);
      break;
    case ::ads9324::CtrlPin::Convst:
      ok = this->DriveOutput(kPinConvst, active);
      break;
    case ::ads9324::CtrlPin::DrdyAlarm:
      ok = false;
      break;
    }
    return ok;
  }
  /** @brief Optional hook: the level of DRDY/ALARM; false for another pin or an absent one. */
  bool GpioGet(::ads9324::CtrlPin pin, bool& high) noexcept {
    bool ok = false;
    high = false;
    if (pin == ::ads9324::CtrlPin::DrdyAlarm) {
      ok = this->ReadInput(kPinAlarm, high);
    }
    return ok;
  }
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::ads9324::ADS9324<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::ads9324::Config chip{}; ///< What EnsureInitialized() writes.
  HandlerConfig handler{};  ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{
      400U}; ///< Bound of the start-up stage (30 ms power-up, calibration).
  std::array<OutputPin, 2U> outputs{}; ///< RESET (`kPinReset`) and CONVST (`kPinConvst`).
  std::array<InputPin, 1U> inputs{};   ///< DRDY/ALARM (`kPinAlarm`).
  bool host_convst{true}; ///< Measure() pulses CONVST (`MeasureOnce()`); false: the board drives
                          ///< CONVST and Measure() waits for data ready, then reads the frame.
};

/** @brief What a sample publishes: one decoded data frame. */
struct Snapshot {
  std::array<int32_t, 16U> microvolts{}; ///< Input voltage per channel (AIN1 at 0).
  std::array<int16_t, 16U> codes{};      ///< Signed codes per channel.
  uint16_t present_mask{0U};             ///< Bit n: channel n is in the frame.
  uint16_t clipped_mask{0U};             ///< Bit n: channel n is at a code limit.
  bool window_alert{false};              ///< DIAG: some window comparator alerts.
  bool standby{false}; ///< The chip is powered down: the values are the last frame.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  ReadWindowFlags = 0,     ///< Latched window flags (`Reply::high`, `Reply::low`).
  ClearWindowFlags = 1,    ///< Clear the flags of `group`.
  RunCalibration = 2,      ///< ADC calibration with `chip.calibration`.
  DisableCalibration = 3,  ///< ADC_CAL_MODE 0.
  PowerDown = 4,           ///< Enter standby.
  Wake = 5,                ///< Leave standby and check the registers.
  Reset = 6,               ///< Reset with `reset` and write the configuration again.
  Identify = 7,            ///< DEVICE_ID (`Reply::value`).
  ReadAlarmPin = 8,        ///< DRDY/ALARM asserted (`Reply::flag`).
  RunDataPathSelfTest = 9, ///< Test patterns on every channel; the configuration is restored.
  Convert = 10,            ///< One CONVST pulse (read the frame with Sample()).
  ReadRegister = 11,       ///< Register `address` of `bank` (`Reply::value`).
};

/** @brief One command. */
struct Command {
  Op op{Op::Identify};                                        ///< What to do.
  uint8_t group{0U};                                          ///< For ClearWindowFlags: 0 or 1.
  uint8_t address{0U};                                        ///< For ReadRegister.
  ::ads9324::reg::Bank bank{::ads9324::reg::Bank::Common};    ///< For ReadRegister.
  ::ads9324::ResetKind reset{::ads9324::ResetKind::Software}; ///< For Reset.
};

/** @brief The reply of a command. */
struct Reply {
  uint16_t value{0U}; ///< DEVICE_ID or the register; 0 otherwise.
  uint16_t high{0U};  ///< Window flags above the high threshold, bit n = channel n.
  uint16_t low{0U};   ///< Window flags below the low threshold.
  bool flag{false};   ///< The alarm pin is asserted.
};

/**
 * @brief The ads9324 handler.
 * @tparam Dev The SPI device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The driver drives RESET itself (`chip.hardware_reset`), so there is no HostReset stage.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Sampled snapshots; occasional calls work too.
  static constexpr BusKind kBus = BusKind::Spi; ///< On an SPI device.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Dev& dev, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(dev, timing, config.outputs, config.inputs), config_(config) {}

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
    return (detail == DetailOf(::ads9324::Error::UnexpectedReset)) ||
           (detail == DetailOf(::ads9324::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Snapshot> result = kb::Result<Snapshot>::Success(last_);
    if (driver.IsPoweredDown()) {
      last_.standby = true;
      result = kb::Result<Snapshot>::Success(last_);
    } else if (config_.host_convst) {
      result = keep(Lift(driver.MeasureOnce(), &ToStatus));
    } else {
      const kb::Result<void> ready = LiftStatus(driver.WaitDataReady(), &ToStatus);
      result = ready.IsOk() ? keep(Lift(driver.ReadFrame(), &ToStatus)) : Forward<Snapshot>(ready);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::ReadWindowFlags:
      result = window(Lift(driver.ReadWindowFlags(), &ToStatus));
      break;
    case Op::ClearWindowFlags:
      result = done(LiftStatus(driver.ClearWindowFlags(c.group), &ToStatus));
      break;
    case Op::RunCalibration:
      result = done(LiftStatus(driver.RunAdcCalibration(), &ToStatus));
      break;
    case Op::DisableCalibration:
      result = done(LiftStatus(driver.DisableAdcCalibration(), &ToStatus));
      break;
    case Op::PowerDown:
      result = done(LiftStatus(driver.PowerDown(), &ToStatus));
      break;
    case Op::Wake:
      result = done(LiftStatus(driver.Wake(), &ToStatus));
      break;
    case Op::Reset:
      result = done(LiftStatus(driver.Reset(c.reset), &ToStatus));
      break;
    case Op::Identify:
      result = value(Lift(driver.Identify(), &ToStatus));
      break;
    case Op::ReadAlarmPin:
      result = flag(Lift(driver.ReadAlarmPin(), &ToStatus));
      break;
    case Op::RunDataPathSelfTest:
      result = done(LiftStatus(driver.RunDataPathSelfTest(), &ToStatus));
      break;
    case Op::Convert:
      result = done(LiftStatus(driver.Convert(), &ToStatus));
      break;
    case Op::ReadRegister:
      result = value(Lift(driver.ReadRegister(c.bank, c.address), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The SPI transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.SpiHooks<Port<Dev>, Dev>::Counters();
  }

private:
  // Decodes a frame into the snapshot, kept for the standby republish.
  [[nodiscard]] kb::Result<Snapshot> keep(const kb::Result<::ads9324::Frame>& r) noexcept {
    kb::Result<Snapshot> result = Forward<Snapshot>(r);
    if (r.IsOk()) {
      const ::ads9324::Frame& f = r.Value();
      Snapshot s{};
      s.microvolts = f.microvolts;
      s.codes = f.codes;
      s.present_mask = f.present_mask;
      s.clipped_mask = f.clipped_mask;
      s.window_alert = f.window_alert;
      last_ = s;
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
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
  [[nodiscard]] static kb::Result<Reply> flag(const kb::Result<bool>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.flag = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> window(
      const kb::Result<::ads9324::WindowFlags>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.high = r.Value().high;
      reply.low = r.Value().low;
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Dev> port_;
  Config config_;
  Snapshot last_{};
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief DEVICE_ID, read now (0x0004). */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> Identify(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Identify;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/** @brief The latched window flags of both banks. */
template <typename Access>
[[nodiscard]] kb::Result<::ads9324::WindowFlags> ReadWindowFlags(
    Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadWindowFlags;
  const kb::Result<Reply> r = io.Request(c, timeout);
  kb::Result<::ads9324::WindowFlags> result = Forward<::ads9324::WindowFlags>(r);
  if (r.IsOk()) {
    ::ads9324::WindowFlags f{};
    f.high = r.Value().high;
    f.low = r.Value().low;
    result = kb::Result<::ads9324::WindowFlags>::Success(f);
  }
  return result;
}

/** @brief Clear the latched window flags of `group` (0: AIN1 to AIN8, 1: AIN9 to AIN16). */
template <typename Access>
[[nodiscard]] kb::Result<void> ClearWindowFlags(Access& io, uint8_t group,
                                                kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ClearWindowFlags;
  c.group = group;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Enter standby (`true`) or leave it (`false`). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetStandby(Access& io, bool standby,
                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = standby ? Op::PowerDown : Op::Wake;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Run the ADC calibration configured in `chip.calibration`. */
template <typename Access>
[[nodiscard]] kb::Result<void> RunCalibration(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::RunCalibration;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Check the data path with test patterns on every channel. */
template <typename Access>
[[nodiscard]] kb::Result<void> RunSelfTest(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::RunDataPathSelfTest;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::ads9324

#endif // KB_HANDLERS_BINDINGS_ADS9324_HPP
