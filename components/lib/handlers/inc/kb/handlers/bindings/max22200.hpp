// clang-format off: the table below must keep one row per line.
/**
 * @file max22200.hpp
 * @brief The max22200 binding and handler: MAX22200 / MAX22200A eight-channel SPI solenoid and
 * valve driver.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks) and its control pins (GpioHooks: ENABLE and CMD out, FAULT in), with `DelayUs` through the hook timing (a spin below 1 ms, a sleep from 1 ms). A board without an ENABLE pin (tied high) leaves it absent |
 * | Errors | `ToStatus()`: every `max22200::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Identify + Configure` = `driver.EnsureInitialized()` (sync): CMD idle, ENABLE high, the Figure 6 programming procedure with the wake poll (`chip.wake_timeout_us`), every register written and read back, start-up faults cleared; every channel off |
 * | Thread model | Guarded (`kRequiresOwner = false`): switched by commands from a control task, supervised by `Sample()`; an owner bus works too, with STATUS read every `sample_period` |
 * | Health | `DeviceReset` (ACTIVE read 0: the registers were lost) and `NotInitialized` fault at once; other failures count to the threshold |
 * | App API | snapshot `{STATUS, latched faults, commanded channels}`; commands: channels on (mask, one, bridge, all off), hold code, pair mode, faults read and clear, FAULT pin, raw register |
 *
 * Safety: the handler only reports. A Faulted health record does not switch any output; what the
 * outputs do on a chip or bus fault is the driver's and the chip's business (the driver sends
 * nothing more after a reset it sees). `Recover()` and `Stop()` call `Deinitialize()`, which
 * switches every channel off and drives ENABLE low before the plan runs again; a safety layer must
 * reach its safe state without any handler call succeeding.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Guarded> spi3{dev};
 * kb::handlers::Guarded<kb::handlers::max22200::Handler<Dev>, decltype(spi3)> valves{spi3, dev, timing, cfg};
 * if (valves.EnsureStarted(kb::Milliseconds{200U}).IsOk()) {
 *   static_cast<void>(kb::handlers::max22200::SetChannelsOn(valves, 0x05U, kb::Milliseconds{5U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_MAX22200_HPP
#define KB_HANDLERS_BINDINGS_MAX22200_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <max22200.hpp>

namespace kb::handlers::max22200 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::max22200::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::max22200::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::max22200::Error::NotInitialized:
  case ::max22200::Error::DeviceReset:
  case ::max22200::Error::Undervoltage:
    s = kb::Status::NotReady;
    break;
  case ::max22200::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::max22200::Error::Bus:
  case ::max22200::Error::Pin:
  case ::max22200::Error::CommunicationError:
    s = kb::Status::BusError;
    break;
  case ::max22200::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::max22200::Error::DeviceIdMismatch:
  case ::max22200::Error::DeviceFault:
  case ::max22200::Error::WriteMismatch:
  case ::max22200::Error::Uncertain:
    s = kb::Status::Fault;
    break;
  case ::max22200::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::max22200::Error::Busy:
  case ::max22200::Error::ChannelsActive:
    s = kb::Status::Busy;
    break;
  case ::max22200::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::max22200::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::max22200::Error error) noexcept {
  return (error == ::max22200::Error::DeviceReset) || (error == ::max22200::Error::NotInitialized);
}

/// @name Control pin indices of the HAL's pin arrays
/// @{
inline constexpr std::size_t kPinEnable =
    0U;                                    ///< Output: ENABLE (active high); absent when tied high.
inline constexpr std::size_t kPinCmd = 1U; ///< Output: CMD (active high, idles high); required.
inline constexpr std::size_t kPinFault = 0U; ///< Input: FAULT (open drain; wire it active low).
/// @}

/**
 * @brief The driver's SPI port and control pins over kb contracts.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef): mode 0, SDO pulled up.
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::max22200::SpiInterface<Port<Dev>>,
             public SpiHooks<Port<Dev>, Dev>,
             public GpioHooks<Port<Dev>, 2U, 1U> {
public:
  /** @brief Over `dev`, `timing` and the HAL's pins; no I/O. */
  Port(Dev& dev, const HookTiming& timing, const std::array<OutputPin, 2U>& outputs,
       const std::array<InputPin, 1U>& inputs) noexcept
      : SpiHooks<Port<Dev>, Dev>(dev, timing), GpioHooks<Port<Dev>, 2U, 1U>(outputs, inputs) {}

  /** @brief Hook: bring the device up; the first call drives CMD high and ENABLE low (idle). */
  bool EnsureInitialized() noexcept {
    bool ok = this->BusReady();
    if (ok && (!idle_set_)) {
      ok = this->DriveOutput(kPinCmd, true);
      if (ok && this->HasOutput(kPinEnable)) {
        ok = this->DriveOutput(kPinEnable, false);
      }
      idle_set_ = ok;
    }
    return ok;
  }
  /** @brief Hook: one full-duplex transfer of 1 or 4 bytes. */
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
  /** @brief Hook: drive CMD or ENABLE; an absent ENABLE is tied high (true). */
  bool GpioSet(::max22200::CtrlPin pin, ::max22200::GpioSignal signal) noexcept {
    const bool active = signal == ::max22200::GpioSignal::ACTIVE;
    bool ok = false;
    switch (pin) {
    case ::max22200::CtrlPin::Enable:
      ok = true;
      if (this->HasOutput(kPinEnable)) {
        ok = this->DriveOutput(kPinEnable, active);
      }
      break;
    case ::max22200::CtrlPin::Cmd:
      ok = this->DriveOutput(kPinCmd, active);
      break;
    case ::max22200::CtrlPin::Fault:
      ok = false;
      break;
    }
    return ok;
  }
  /** @brief Optional hook: the FAULT pin, asserted while an unmasked fault is present. */
  bool GpioRead(::max22200::CtrlPin pin, ::max22200::GpioSignal& signal) noexcept {
    bool ok = false;
    bool asserted = false;
    if (pin == ::max22200::CtrlPin::Fault) {
      ok = this->ReadInput(kPinFault, asserted);
    }
    signal = asserted ? ::max22200::GpioSignal::ACTIVE : ::max22200::GpioSignal::INACTIVE;
    return ok;
  }

private:
  bool idle_set_{false};
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::max22200::MAX22200<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::max22200::Config chip{}; ///< What EnsureInitialized() writes (channels, masks, Rref).
  HandlerConfig handler{};   ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{500U}; ///< Bound of the start-up stage (covers the wake poll).
  std::array<OutputPin, 2U> outputs{};   ///< ENABLE (`kPinEnable`) and CMD (`kPinCmd`).
  std::array<InputPin, 1U> inputs{};     ///< FAULT (`kPinFault`).
};

/** @brief What a sample publishes: STATUS as read, and what the driver latched. */
struct Snapshot {
  ::max22200::Status status{}; ///< STATUS (ONCH, flags, ACTIVE); OVT and UVM cleared by the read.
  ::max22200::LatchedFaults latched{}; ///< Flags any read took from the chip since the last clear.
  uint8_t commanded{0U};               ///< ONCH as the driver last wrote it, bit n = channel n.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SetChannelsOn = 0,  ///< Every channel at once: `mask` bits on.
  SetChannel = 1,     ///< Channel `channel` on (`on`) or off.
  SetBridge = 2,      ///< Full-bridge pair `channel` to `bridge`.
  AllOff = 3,         ///< Every channel off.
  SetHoldCode = 4,    ///< HOLD of channel `channel` to `code`.
  SetPairMode = 5,    ///< Pair `channel` to `pair_mode` (channels of the pair off).
  ReadChannelsOn = 6, ///< ONCH from the chip (`Reply::value`).
  ReadStatus = 7,     ///< STATUS (`Reply::value`: the raw register).
  ReadFaults = 8,     ///< FAULT without clearing (`Reply::faults`).
  ClearFaults = 9,    ///< FAULT read and every flag cleared (`Reply::faults`: before the clear).
  ReadFaultPin = 10,  ///< The FAULT pin (`Reply::flag`).
  ClearLatchedFaults = 11, ///< Empty the driver's fault latch (no I/O).
  ReadRegister = 12,       ///< Register bank `channel` (`Reply::value`).
};

/** @brief One command. */
struct Command {
  Op op{Op::ReadStatus};                                             ///< What to do.
  uint8_t mask{0U};                                                  ///< For SetChannelsOn.
  uint8_t channel{0U};                                               ///< Channel, pair or bank.
  uint8_t code{0U};                                                  ///< For SetHoldCode.
  bool on{false};                                                    ///< For SetChannel.
  ::max22200::BridgeState bridge{::max22200::BridgeState::HiZ};      ///< For SetBridge.
  ::max22200::PairMode pair_mode{::max22200::PairMode::Independent}; ///< For SetPairMode.
  ::max22200::Verify verify{::max22200::Verify::None};               ///< Read back after writing.
};

/** @brief The reply of a command. */
struct Reply {
  uint32_t value{0U};          ///< ONCH, STATUS or a register; 0 otherwise.
  ::max22200::Faults faults{}; ///< For ReadFaults and ClearFaults.
  bool flag{false};            ///< For ReadFaultPin: asserted.
};

/**
 * @brief The max22200 handler.
 * @tparam Dev The SPI device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The driver drives ENABLE itself, so there is no Power stage.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by commands and occasional samples.
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
    return (detail == DetailOf(::max22200::Error::DeviceReset)) ||
           (detail == DetailOf(::max22200::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<::max22200::Status> st = Lift(driver.ReadStatus(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(st);
    if (st.IsOk()) {
      Snapshot s{};
      s.status = st.Value();
      s.latched = driver.GetLatchedFaults();
      s.commanded = driver.GetChannelsOn();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SetChannelsOn:
      result = done(LiftStatus(driver.SetChannelsOn(c.mask, c.verify), &ToStatus));
      break;
    case Op::SetChannel:
      result = done(LiftStatus(driver.SetChannel(c.channel, c.on, c.verify), &ToStatus));
      break;
    case Op::SetBridge:
      result = done(LiftStatus(driver.SetBridge(c.channel, c.bridge, c.verify), &ToStatus));
      break;
    case Op::AllOff:
      result = done(LiftStatus(driver.SetChannelsOn(0U, c.verify), &ToStatus));
      break;
    case Op::SetHoldCode:
      result = done(LiftStatus(driver.SetHoldCode(c.channel, c.code, c.verify), &ToStatus));
      break;
    case Op::SetPairMode:
      result = done(LiftStatus(driver.SetPairMode(c.channel, c.pair_mode, c.verify), &ToStatus));
      break;
    case Op::ReadChannelsOn:
      result = word(Lift(driver.ReadChannelsOn(), &ToStatus));
      break;
    case Op::ReadStatus:
      result = status(Lift(driver.ReadStatus(), &ToStatus));
      break;
    case Op::ReadFaults:
      result = faults(Lift(driver.ReadFaults(), &ToStatus));
      break;
    case Op::ClearFaults:
      result = faults(Lift(driver.ClearFaults(::max22200::Faults::All()), &ToStatus));
      break;
    case Op::ReadFaultPin:
      result = flag(Lift(driver.ReadFaultPin(), &ToStatus));
      break;
    case Op::ClearLatchedFaults:
      driver.ClearLatchedFaults();
      result = kb::Result<Reply>::Success(Reply{});
      break;
    case Op::ReadRegister:
      result = word(Lift(driver.ReadRegister(c.channel), &ToStatus));
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
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  template <typename T>
  [[nodiscard]] static kb::Result<Reply> word(const kb::Result<T>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = static_cast<uint32_t>(r.Value());
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> status(const kb::Result<::max22200::Status>& r) noexcept {
    return r.IsOk() ? word(kb::Result<uint32_t>::Success(r.Value().raw)) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> faults(const kb::Result<::max22200::Faults>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.faults = r.Value();
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

/** @brief Every channel at once: bit n of `mask` switches channel n on. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetChannelsOn(
    Access& io, uint8_t mask, kb::Milliseconds timeout,
    ::max22200::Verify verify = ::max22200::Verify::None) noexcept {
  Command c{};
  c.op = Op::SetChannelsOn;
  c.mask = mask;
  c.verify = verify;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief One channel on or off, the others as the driver last wrote them. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetChannel(Access& io, uint8_t channel, bool on,
                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetChannel;
  c.channel = channel;
  c.on = on;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Every channel off (read back). */
template <typename Access>
[[nodiscard]] kb::Result<void> AllOff(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::AllOff;
  c.verify = ::max22200::Verify::ReadBack;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The per-channel FAULT flags, read without clearing. */
template <typename Access>
[[nodiscard]] kb::Result<::max22200::Faults> ReadFaults(Access& io,
                                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFaults;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::max22200::Faults>::Success(r.Value().faults)
                  : Forward<::max22200::Faults>(r);
}

/** @brief Clear every FAULT flag; returns the flags as they were before. */
template <typename Access>
[[nodiscard]] kb::Result<::max22200::Faults> ClearFaults(Access& io,
                                                         kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ClearFaults;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::max22200::Faults>::Success(r.Value().faults)
                  : Forward<::max22200::Faults>(r);
}

/** @brief The FAULT pin: true while an unmasked fault is present. */
template <typename Access>
[[nodiscard]] kb::Result<bool> ReadFaultPin(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadFaultPin;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<bool>::Success(r.Value().flag) : Forward<bool>(r);
}

/// @}

} // namespace kb::handlers::max22200

#endif // KB_HANDLERS_BINDINGS_MAX22200_HPP
