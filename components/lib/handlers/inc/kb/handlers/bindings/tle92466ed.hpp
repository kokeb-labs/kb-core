// clang-format off: the table below must keep one row per line.
/**
 * @file tle92466ed.hpp
 * @brief The tle92466ed binding and handler: TLE92466ED six-channel SPI solenoid current
 * controller.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks) and its control pins (GpioHooks: RESN, EN, DRV0, DRV1 out, FAULTN in), with `DelayUs` through the hook timing (a spin below 1 ms, a sleep from 1 ms). A pin the board does not wire is left absent: driving it succeeds without doing anything, as the driver expects |
 * | Errors | `ToStatus()`: every `tle92466ed::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Identify + Configure` = `driver.EnsureInitialized()` (sync): reset (RESN pulse, or CH_CTRL 0 without RESN), INIT_DONE polled (`chip.init_timeout_us`), ICVID, diagnosis kept and cleared, every register written, SPI watchdog last; ends in Config Mode with every channel off |
 * | Thread model | Guarded (`kRequiresOwner = false`) for commanded valves; with `chip.spi_watchdog` on, use an owner bus whose `sample_period` is shorter than the watchdog time: `Measure()` reloads the watchdog before it reads FB_STAT |
 * | Health | `UnexpectedReset` (POR or RES event, also when a sampled FB_STAT shows one), `CriticalFault` (the chip is in its Critical Fault state) and `NotInitialized` fault at once; other failures count to the threshold |
 * | App API | snapshot `{FB_STAT, enabled, dropped, mission}`; commands: mission and config mode, channels (mask, one on, one off, all off), current setpoint, feedback, faults, watchdog, DRV pins, supply, FAULTN, raw register |
 *
 * Safety: the handler only reports. A Faulted health record does not switch any output; on a
 * reset or a critical fault the driver itself drives EN low and stops. `Recover()` and `Stop()`
 * call `Deinitialize()` (Config Mode, every channel off, EN low) before the plan runs again; a
 * safety layer must reach its safe state without any handler call succeeding.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Guarded> spi4{dev};
 * kb::handlers::Guarded<kb::handlers::tle92466ed::Handler<Dev>, decltype(spi4)> icc{spi4, dev, timing, cfg};
 * if (icc.EnsureStarted(kb::Milliseconds{100U}).IsOk()) {
 *   static_cast<void>(kb::handlers::tle92466ed::SetCurrent(icc, tle92466ed::Channel::Ch0, 500'000U, kb::Milliseconds{5U}));
 *   static_cast<void>(kb::handlers::tle92466ed::EnterMissionMode(icc, kb::Milliseconds{5U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_TLE92466ED_HPP
#define KB_HANDLERS_BINDINGS_TLE92466ED_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <tle92466ed.hpp>

namespace kb::handlers::tle92466ed {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::tle92466ed::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::tle92466ed::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::tle92466ed::Error::NotInitialized:
  case ::tle92466ed::Error::UnexpectedReset:
  case ::tle92466ed::Error::NotReady:
    s = kb::Status::NotReady;
    break;
  case ::tle92466ed::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::tle92466ed::Error::Bus:
  case ::tle92466ed::Error::ReplyMismatch:
  case ::tle92466ed::Error::FrameRejected:
    s = kb::Status::BusError;
    break;
  case ::tle92466ed::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::tle92466ed::Error::DeviceIdMismatch:
  case ::tle92466ed::Error::DeviceFault:
  case ::tle92466ed::Error::CriticalFault:
  case ::tle92466ed::Error::MissionBlocked:
  case ::tle92466ed::Error::WriteMismatch:
    s = kb::Status::Fault;
    break;
  case ::tle92466ed::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::tle92466ed::Error::Busy:
  case ::tle92466ed::Error::WrongState:
    s = kb::Status::Busy;
    break;
  case ::tle92466ed::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::tle92466ed::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::tle92466ed::Error error) noexcept {
  return (error == ::tle92466ed::Error::UnexpectedReset) ||
         (error == ::tle92466ed::Error::CriticalFault) ||
         (error == ::tle92466ed::Error::NotInitialized);
}

/// @name Control pin indices of the HAL's pin arrays
/// @{
inline constexpr std::size_t kPinReset = 0U;  ///< Output: RESN (wire it active low).
inline constexpr std::size_t kPinEnable = 1U; ///< Output: EN (active high).
inline constexpr std::size_t kPinDrv0 = 2U;   ///< Output: DRV0 (active high).
inline constexpr std::size_t kPinDrv1 = 3U;   ///< Output: DRV1 (active high).
inline constexpr std::size_t kPinFault = 0U;  ///< Input: FAULTN (open drain; wire it active low).
/// @}

/** @brief The output index of a driver pin; `Outputs` for FAULTN (an input). */
[[nodiscard]] constexpr std::size_t OutputIndex(::tle92466ed::CtrlPin pin) noexcept {
  std::size_t index = 4U;
  switch (pin) {
  case ::tle92466ed::CtrlPin::Reset:
    index = kPinReset;
    break;
  case ::tle92466ed::CtrlPin::Enable:
    index = kPinEnable;
    break;
  case ::tle92466ed::CtrlPin::Drv0:
    index = kPinDrv0;
    break;
  case ::tle92466ed::CtrlPin::Drv1:
    index = kPinDrv1;
    break;
  case ::tle92466ed::CtrlPin::Fault:
    index = 4U;
    break;
  }
  return index;
}

/**
 * @brief The driver's SPI port and control pins over kb contracts.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef): mode 1, 32-bit frames.
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::tle92466ed::SpiInterface<Port<Dev>>,
             public SpiHooks<Port<Dev>, Dev>,
             public GpioHooks<Port<Dev>, 4U, 1U> {
public:
  /** @brief Over `dev`, `timing` and the HAL's pins; no I/O. */
  Port(Dev& dev, const HookTiming& timing, const std::array<OutputPin, 4U>& outputs,
       const std::array<InputPin, 1U>& inputs) noexcept
      : SpiHooks<Port<Dev>, Dev>(dev, timing), GpioHooks<Port<Dev>, 4U, 1U>(outputs, inputs) {}

  /** @brief Hook: bring the device up; the first call drives each wired output to its idle level.
   */
  bool EnsureInitialized() noexcept {
    bool ok = this->BusReady();
    if (ok && (!idle_set_)) {
      ok = idle(kPinReset, true); // RESN idles in reset
      ok = ok ? idle(kPinEnable, false) : false;
      ok = ok ? idle(kPinDrv0, false) : false;
      ok = ok ? idle(kPinDrv1, false) : false;
      idle_set_ = ok;
    }
    return ok;
  }
  /** @brief Hook: one full-duplex 32-bit frame. */
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
  /** @brief Optional hook: drive RESN, EN, DRV0 or DRV1; an unwired pin succeeds doing nothing. */
  bool GpioSet(::tle92466ed::CtrlPin pin, ::tle92466ed::GpioSignal signal) noexcept {
    return idle(OutputIndex(pin), signal == ::tle92466ed::GpioSignal::ACTIVE);
  }
  /** @brief Optional hook: FAULTN, asserted while a fault is reported. */
  bool GpioRead(::tle92466ed::CtrlPin pin, ::tle92466ed::GpioSignal& signal) noexcept {
    bool ok = false;
    bool asserted = false;
    if (pin == ::tle92466ed::CtrlPin::Fault) {
      ok = this->ReadInput(kPinFault, asserted);
    }
    signal = asserted ? ::tle92466ed::GpioSignal::ACTIVE : ::tle92466ed::GpioSignal::INACTIVE;
    return ok;
  }

private:
  // Drives output `index` when the board wires it; true for an unwired output, false for none.
  [[nodiscard]] bool idle(std::size_t index, bool asserted) noexcept {
    bool ok = index < 4U;
    if (ok && this->HasOutput(index)) {
      ok = this->DriveOutput(index, asserted);
    }
    return ok;
  }
  bool idle_set_{false};
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::tle92466ed::TLE92466ED<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::tle92466ed::Config chip{}; ///< What EnsureInitialized() writes (channels, VIO, watchdog).
  HandlerConfig handler{};     ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{
      200U}; ///< Bound of the start-up stage (reset, INIT_DONE, 181 frames).
  std::array<OutputPin, 4U> outputs{}; ///< RESN, EN, DRV0, DRV1 (`kPinReset` to `kPinDrv1`).
  std::array<InputPin, 1U> inputs{};   ///< FAULTN (`kPinFault`).
};

/** @brief What a sample publishes: FB_STAT and the driver's view of the channels. */
struct Snapshot {
  ::tle92466ed::DeviceStatus status{}; ///< FB_STAT, decoded.
  uint8_t enabled{0U};                 ///< EN_CH bits of the driver's copy of CH_CTRL.
  uint8_t dropped{0U}; ///< Channels the chip switched off on its own (since a clear).
  bool mission{false}; ///< The driver last set Mission Mode and saw nothing leave it.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  EnterMissionMode = 0,   ///< Mission Mode, when the entry conditions hold.
  EnterConfigMode = 1,    ///< Config Mode: every channel off.
  SetEnabledChannels = 2, ///< Channels in `mask` on, the others off.
  EnableChannel = 3,      ///< Channel `channel` on.
  DisableChannel = 4,     ///< Channel `channel` off (any state).
  AllOff = 5,             ///< Every channel off.
  SetCurrent = 6,         ///< Setpoint of `channel` to `microamps` (`Reply::value`: programmed).
  ReadFeedback = 7,       ///< Feedback of `channel` (`Reply::current_ua`, `value`: duty per 10000).
  ReadFaults = 8,         ///< The nine diagnosis registers (`Reply::faults`).
  ClearFaults = 9,        ///< Clear the diagnosis flags on the chip.
  ReloadWatchdog = 10,    ///< Reload the SPI watchdog.
  SetDrivePin = 11,       ///< DRV0 (`channel` 0) or DRV1 (1) to `high`.
  ReadSupply = 12,   ///< VBAT (`Reply::value`, mV) and temperature (`Reply::current_ua`, mdegC).
  ReadFaultPin = 13, ///< FAULTN asserted (`Reply::flag`).
  ReadRegister = 14, ///< Register `address` (`Reply::value`).
  ClearRecords = 15, ///< Empty the dropped-channel record and the fault latch (no I/O).
};

/** @brief One command. */
struct Command {
  Op op{Op::DisableChannel};                               ///< What to do.
  uint8_t mask{0U};                                        ///< For SetEnabledChannels.
  uint8_t channel{0U};                                     ///< Channel 0 to 5, or the DRV pin.
  bool high{false};                                        ///< For SetDrivePin.
  uint16_t address{0U};                                    ///< For ReadRegister.
  uint32_t microamps{0U};                                  ///< For SetCurrent.
  ::tle92466ed::Verify verify{::tle92466ed::Verify::None}; ///< Read back after writing.
};

/** @brief The reply of a command. */
struct Reply {
  uint32_t value{0U};            ///< Setpoint, duty, VBAT or a register; 0 otherwise.
  int32_t current_ua{0};         ///< Feedback current, or the temperature of ReadSupply.
  ::tle92466ed::Faults faults{}; ///< For ReadFaults.
  bool flag{false};              ///< For ReadFaultPin.
};

/**
 * @brief The tle92466ed handler.
 * @tparam Dev The SPI device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The driver drives RESN and EN itself, so there is no HostReset or Power stage.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Commands; an owner serves the SPI watchdog.
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
    return (detail == DetailOf(::tle92466ed::Error::UnexpectedReset)) ||
           (detail == DetailOf(::tle92466ed::Error::CriticalFault)) ||
           (detail == DetailOf(::tle92466ed::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<void> fed = kb::Result<void>::Success();
    if (config_.chip.spi_watchdog) {
      fed = LiftStatus(driver.ReloadWatchdog(), &ToStatus);
    }
    kb::Result<Snapshot> result = Forward<Snapshot>(fed);
    if (fed.IsOk()) {
      const kb::Result<::tle92466ed::DeviceStatus> st = Lift(driver.ReadStatus(), &ToStatus);
      result = Forward<Snapshot>(st);
      const bool events = st.IsOk() && (st.Value().power_on_event || st.Value().reset_event);
      if (events && driver.IsResetWatchArmed()) {
        // FB_STAT read as data shows the reset the driver's anchors look for: report it as one.
        constexpr ::tle92466ed::Error kReset = ::tle92466ed::Error::UnexpectedReset;
        result = kb::Result<Snapshot>::Failure(ToStatus(kReset), DetailOf(kReset));
      } else if (st.IsOk()) {
        Snapshot s{};
        s.status = st.Value();
        s.enabled = driver.GetEnabledChannels();
        s.dropped = driver.GetDroppedChannels();
        s.mission = driver.IsMissionMode();
        result = kb::Result<Snapshot>::Success(s);
      } else {
        // The read failed: `result` carries its status and code.
      }
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::EnterMissionMode:
      result = done(LiftStatus(driver.EnterMissionMode(), &ToStatus));
      break;
    case Op::EnterConfigMode:
      result = done(LiftStatus(driver.EnterConfigMode(), &ToStatus));
      break;
    case Op::SetEnabledChannels:
      result = done(LiftStatus(driver.SetEnabledChannels(c.mask, c.verify), &ToStatus));
      break;
    case Op::EnableChannel:
    case Op::DisableChannel:
    case Op::SetCurrent:
    case Op::ReadFeedback:
      result = perChannel(driver, c);
      break;
    case Op::AllOff:
      result = done(LiftStatus(driver.SetEnabledChannels(0U, c.verify), &ToStatus));
      break;
    case Op::ReadFaults:
      result = faults(Lift(driver.ReadFaults(), &ToStatus));
      break;
    case Op::ClearFaults:
      result = done(LiftStatus(driver.ClearFaults(), &ToStatus));
      break;
    case Op::ReloadWatchdog:
      result = done(LiftStatus(driver.ReloadWatchdog(), &ToStatus));
      break;
    case Op::SetDrivePin:
      result = drivePin(driver, c);
      break;
    case Op::ReadSupply:
      result = supply(Lift(driver.ReadSupply(), &ToStatus));
      break;
    case Op::ReadFaultPin:
      result = flag(Lift(driver.ReadFaultPin(), &ToStatus));
      break;
    case Op::ReadRegister:
      result = word(Lift(driver.ReadRegister(c.address), &ToStatus));
      break;
    case Op::ClearRecords:
      driver.ClearDroppedChannels();
      driver.ClearLatchedFaults();
      result = kb::Result<Reply>::Success(Reply{});
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
  // The commands that name a channel; InvalidArgument and nothing sent above Ch5.
  [[nodiscard]] static kb::Result<Reply> perChannel(Driver<Dev>& driver,
                                                    const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    if (c.channel < static_cast<uint8_t>(::tle92466ed::kChannelCount)) {
      const auto ch = static_cast<::tle92466ed::Channel>(c.channel);
      if (c.op == Op::EnableChannel) {
        result = done(LiftStatus(driver.EnableChannel(ch, c.verify), &ToStatus));
      } else if (c.op == Op::DisableChannel) {
        result = done(LiftStatus(driver.DisableChannel(ch, c.verify), &ToStatus));
      } else if (c.op == Op::SetCurrent) {
        result = word(Lift(driver.SetCurrent(ch, c.microamps, c.verify), &ToStatus));
      } else {
        result = feedback(Lift(driver.ReadChannelFeedback(ch), &ToStatus));
      }
    }
    return result;
  }
  // DRV0 (`channel` 0) or DRV1 (`channel` 1).
  [[nodiscard]] static kb::Result<Reply> drivePin(Driver<Dev>& driver, const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    if (c.channel <= 1U) {
      const ::tle92466ed::CtrlPin pin =
          (c.channel == 0U) ? ::tle92466ed::CtrlPin::Drv0 : ::tle92466ed::CtrlPin::Drv1;
      result = done(LiftStatus(driver.SetDrivePin(pin, c.high), &ToStatus));
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> word(const kb::Result<uint32_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> feedback(
      const kb::Result<::tle92466ed::ChannelFeedback>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value().duty_permyriad;
      reply.current_ua = r.Value().average_current_ua;
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> faults(
      const kb::Result<::tle92466ed::Faults>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.faults = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> supply(
      const kb::Result<::tle92466ed::SupplyReadings>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value().vbat_mv;
      reply.current_ua = r.Value().temperature_mc;
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

  Port<Dev> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Enter Mission Mode (the enabled channels switch). */
template <typename Access>
[[nodiscard]] kb::Result<void> EnterMissionMode(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::EnterMissionMode;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Enter Config Mode: every channel off. */
template <typename Access>
[[nodiscard]] kb::Result<void> EnterConfigMode(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::EnterConfigMode;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The channels in `mask` on (bit x = channel x), the others off. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetEnabledChannels(
    Access& io, uint8_t mask, kb::Milliseconds timeout,
    ::tle92466ed::Verify verify = ::tle92466ed::Verify::None) noexcept {
  Command c{};
  c.op = Op::SetEnabledChannels;
  c.mask = mask;
  c.verify = verify;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Channel `channel` off, whatever the state. */
template <typename Access>
[[nodiscard]] kb::Result<void> DisableChannel(Access& io, ::tle92466ed::Channel channel,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisableChannel;
  c.channel = static_cast<uint8_t>(channel);
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The average current setpoint of `channel`; returns the programmed microamps. */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> SetCurrent(Access& io, ::tle92466ed::Channel channel,
                                              uint32_t microamps,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetCurrent;
  c.channel = static_cast<uint8_t>(channel);
  c.microamps = microamps;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().value) : Forward<uint32_t>(r);
}

/** @brief The average current of `channel` over its last measurement period, in microamps. */
template <typename Access>
[[nodiscard]] kb::Result<int32_t> ReadCurrent(Access& io, ::tle92466ed::Channel channel,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFeedback;
  c.channel = static_cast<uint8_t>(channel);
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int32_t>::Success(r.Value().current_ua) : Forward<int32_t>(r);
}

/** @brief The nine diagnosis registers. */
template <typename Access>
[[nodiscard]] kb::Result<::tle92466ed::Faults> ReadFaults(Access& io,
                                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFaults;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::tle92466ed::Faults>::Success(r.Value().faults)
                  : Forward<::tle92466ed::Faults>(r);
}

/** @brief Clear the diagnosis flags on the chip. */
template <typename Access>
[[nodiscard]] kb::Result<void> ClearFaults(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ClearFaults;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::tle92466ed

#endif // KB_HANDLERS_BINDINGS_TLE92466ED_HPP
