/**
 * @file tmc9660.hpp
 * @brief The tmc9660 binding and handler: TMC9660 servo controller and gate driver, on SPI or UART,
 * in parameter, register or bootloader mode.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `PortOf<Bus, kControlPins>`: `SpiPort` over a kb::iface::SpiDevice (SpiHooks) or
 * `UartPort` over a kb::iface::UartPort (UartHooks: flush, write, read; a send of 0 bytes only
 * drains the line), `DelayUs` through the hook timing; with `kControlPins` also `GpioSet`/`GpioGet`
 * over GpioHooks (RSTN, DRV_EN, WAKE; FAULTN) | | Errors | `ToStatus()`: every `tmc9660::Error` to
 * a kb::Status; the code in the health record's detail | | Start-up | `Identify + Configure` =
 * `driver.EnsureInitialized()` (sync): bus up, optional RSTN pulse (`Config::chip.hardware_reset`,
 * the driver drives the pin), chip ID in `Config::chip.mode` | | Thread model | Guarded
 * (`kRequiresOwner = false`): set points and parameter access are sporadic and the control loops
 * run in the chip; an owner bus works too and runs `Service()` (heartbeat, reset detection) every
 * `sample_period` | | Health | `UnexpectedReset` (the chip restarted) and `NotInitialized` fault at
 * once; other failures count to the threshold. `Deinitialize()` (the stop and recovery path)
 * switches the gate driver off | | App API | snapshot `{mode, velocity, position, error flags}`;
 * commands: enable and disable the drive, motor stop, targets, parameters, flags, registers |
 *
 * The bus type picks the transport: a kb::iface::SpiDevice is the SPI link, a kb::iface::UartPort
 * the UART link (the port must not return the echo of its own bytes). `kControlPins = false` (the
 * default) gives the driver no pin hooks: no hardware reset, DRV_EN wired high and the stop made by
 * parameters. With `kControlPins = true` the pins come from `Config::pins`; DRV_EN must then be
 * wired (a missing output fails its hook, `Bus` from the driver, and the driver still sends every
 * stop frame).
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Guarded> spi2{dev};
 * kb::handlers::Guarded<kb::handlers::tmc9660::Handler<Dev>, decltype(spi2)> servo{spi2, dev,
 * timing, cfg}; if (servo.EnsureStarted(kb::Milliseconds{600U}).IsOk()) {
 *   static_cast<void>(kb::handlers::tmc9660::EnableDrive(servo, 2U, kb::Milliseconds{50U}));
 *   static_cast<void>(kb::handlers::tmc9660::SetTargetVelocity(servo, 1000,
 * kb::Milliseconds{20U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_TMC9660_HPP
#define KB_HANDLERS_BINDINGS_TMC9660_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <tmc9660.hpp>

namespace kb::handlers::tmc9660 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::tmc9660::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::tmc9660::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::tmc9660::Error::NotInitialized:
  case ::tmc9660::Error::UnexpectedReset:
    s = kb::Status::NotReady;
    break;
  case ::tmc9660::Error::InvalidArgument:
  case ::tmc9660::Error::Refused:
    s = kb::Status::InvalidArgument;
    break;
  case ::tmc9660::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::tmc9660::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::tmc9660::Error::DeviceIdMismatch:
    s = kb::Status::NoDevice;
    break;
  case ::tmc9660::Error::Crc:
  case ::tmc9660::Error::BadReply:
    s = kb::Status::CrcMismatch;
    break;
  case ::tmc9660::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::tmc9660::Error::Nack:
    s = kb::Status::Nack;
    break;
  case ::tmc9660::Error::Unsupported:
  case ::tmc9660::Error::WrongMode:
    s = kb::Status::Unsupported;
    break;
  case ::tmc9660::Error::DeviceFault:
  case ::tmc9660::Error::VerifyFailed:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::tmc9660::Error error) noexcept {
  return (error == ::tmc9660::Error::UnexpectedReset) ||
         (error == ::tmc9660::Error::NotInitialized);
}

/// @name Control pin indices of `ControlPins` (the driver's `CtrlPin` order)
/// @{
inline constexpr std::size_t kPinReset =
    0U; ///< Output: RSTN (asserted = held in reset; active low).
inline constexpr std::size_t kPinDrvEn = 1U;  ///< Output: DRV_EN (asserted = outputs allowed).
inline constexpr std::size_t kPinWake = 2U;   ///< Output: WAKE.
inline constexpr std::size_t kPinFaultn = 0U; ///< Input: FAULTN (asserted = fault; active low).
inline constexpr std::size_t kOutputs = 3U;   ///< Control outputs.
inline constexpr std::size_t kInputs = 1U;    ///< Control inputs.
/// @}

/** @brief The control pins a HAL may supply (board values: pins and polarity). */
struct ControlPins {
  std::array<OutputPin, kOutputs> outputs{}; ///< RSTN (`active_low`), DRV_EN, WAKE.
  std::array<InputPin, kInputs> inputs{};    ///< FAULTN (`active_low`).
};

/** @brief The driver's microsecond timeout as whole milliseconds, rounded up. */
[[nodiscard]] constexpr uint32_t CeilMs(uint32_t us) noexcept {
  return (us / 1000U) + (((us % 1000U) != 0U) ? 1U : 0U);
}

/** @brief The output index of a driver control pin; kOutputs for FAULTN (an input). */
[[nodiscard]] constexpr std::size_t OutputIndex(::tmc9660::CtrlPin pin) noexcept {
  std::size_t index = kOutputs;
  switch (pin) {
  case ::tmc9660::CtrlPin::Reset:
    index = kPinReset;
    break;
  case ::tmc9660::CtrlPin::DrvEn:
    index = kPinDrvEn;
    break;
  case ::tmc9660::CtrlPin::Wake:
    index = kPinWake;
    break;
  case ::tmc9660::CtrlPin::Faultn:
    index = kOutputs;
    break;
  }
  return index;
}

/**
 * @brief The pin hooks both ports share, over GpioHooks: the driver works in pin functions
 * (`GpioSignal`) for outputs and in levels for FAULTN.
 * @tparam Derived The port.
 */
template <typename Derived>
class PinMap : public GpioHooks<Derived, kOutputs, kInputs> {
  using Pins = GpioHooks<Derived, kOutputs, kInputs>;

protected:
  /** @brief Over the board's `pins`; no I/O. */
  explicit PinMap(const ControlPins& pins) noexcept
      : Pins(pins.outputs, pins.inputs), faultn_active_low_(pins.inputs[kPinFaultn].active_low) {}

  /** @brief Drives `pin` (an output) to `signal`; false for FAULTN or a pin the board lacks. */
  [[nodiscard]] bool SetPin(::tmc9660::CtrlPin pin, ::tmc9660::GpioSignal signal) noexcept {
    return this->DriveOutput(OutputIndex(pin), signal == ::tmc9660::GpioSignal::ACTIVE);
  }
  /** @brief The level (`high`) of FAULTN; false for an output pin or a missing input. */
  [[nodiscard]] bool GetPin(::tmc9660::CtrlPin pin, bool& high) noexcept {
    bool asserted = false;
    const bool ok = (pin == ::tmc9660::CtrlPin::Faultn) && this->ReadInput(kPinFaultn, asserted);
    high = asserted != faultn_active_low_;
    return ok;
  }

private:
  bool faultn_active_low_;
};

/**
 * @brief The driver's SPI port over a kb::iface::SpiDevice (one chip select, mode 3).
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 * @tparam kControlPins True: the GpioSet/GpioGet hooks exist and use `ControlPins`.
 */
template <kb::iface::SpiDevice Dev, bool kControlPins>
class SpiPort : public ::tmc9660::SpiInterface<SpiPort<Dev, kControlPins>>,
                public SpiHooks<SpiPort<Dev, kControlPins>, Dev>,
                public PinMap<SpiPort<Dev, kControlPins>> {
  using Bus = SpiHooks<SpiPort<Dev, kControlPins>, Dev>;
  using Pins = PinMap<SpiPort<Dev, kControlPins>>;

public:
  /** @brief Over `dev` with `timing` and the board's `pins`; no I/O. */
  SpiPort(Dev& dev, const HookTiming& timing, const ControlPins& pins) noexcept
      : Bus(dev, timing), Pins(pins) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one full-duplex frame. */
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return this->BusTransfer(tx, rx, len);
  }
  /** @brief Hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: drive RSTN, DRV_EN or WAKE (with `kControlPins`). */
  bool GpioSet(::tmc9660::CtrlPin pin, ::tmc9660::GpioSignal signal) noexcept
    requires(kControlPins)
  {
    return this->SetPin(pin, signal);
  }
  /** @brief Optional hook: the level of FAULTN (with `kControlPins`). */
  bool GpioGet(::tmc9660::CtrlPin pin, bool& high) noexcept
    requires(kControlPins)
  {
    return this->GetPin(pin, high);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Bus::Counters();
  }
};

/**
 * @brief The driver's UART port over a kb::iface::UartPort.
 * @tparam Uart The port (a UartPort or UartPortRef); it must not return the echo of its own bytes.
 * @tparam kControlPins True: the GpioSet/GpioGet hooks exist and use `ControlPins`.
 */
template <kb::iface::UartPort Uart, bool kControlPins>
class UartPort : public ::tmc9660::UartInterface<UartPort<Uart, kControlPins>>,
                 public UartHooks<UartPort<Uart, kControlPins>, Uart>,
                 public PinMap<UartPort<Uart, kControlPins>> {
  using Bus = UartHooks<UartPort<Uart, kControlPins>, Uart>;
  using Pins = PinMap<UartPort<Uart, kControlPins>>;

public:
  /** @brief Over `uart` with `timing` and the board's `pins`; no I/O. */
  UartPort(Uart& uart, const HookTiming& timing, const ControlPins& pins) noexcept
      : Bus(uart, timing), Pins(pins) {}

  /** @brief Hook: bring the port up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /**
   * @brief Hook: drop stale bytes, send `tx`, then receive exactly `rx_len` bytes; with no bytes to
   * send, only drain what waits on the line (up to `rx_len` bytes within the timeout).
   */
  ::tmc9660::BusStatus Exchange(const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                                std::size_t rx_len, uint32_t timeout_us) noexcept {
    ::tmc9660::BusStatus status = ::tmc9660::BusStatus::Fault;
    if (tx_len == 0U) {
      static_cast<void>(this->BusRead(rx, rx_len, CeilMs(timeout_us)));
      status = ::tmc9660::BusStatus::Ok;
    } else if (this->BusFlush() && this->BusWrite(tx, tx_len)) {
      const bool all = this->BusRead(rx, rx_len, CeilMs(timeout_us)) == rx_len;
      status = all ? ::tmc9660::BusStatus::Ok : ::tmc9660::BusStatus::Timeout;
    } else {
      status = ::tmc9660::BusStatus::Fault;
    }
    return status;
  }
  /** @brief Hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: drive RSTN, DRV_EN or WAKE (with `kControlPins`). */
  bool GpioSet(::tmc9660::CtrlPin pin, ::tmc9660::GpioSignal signal) noexcept
    requires(kControlPins)
  {
    return this->SetPin(pin, signal);
  }
  /** @brief Optional hook: the level of FAULTN (with `kControlPins`). */
  bool GpioGet(::tmc9660::CtrlPin pin, bool& high) noexcept
    requires(kControlPins)
  {
    return this->GetPin(pin, high);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Bus::Counters();
  }
};

/** @brief A bus the binding can serve: a SPI device or a UART port. */
template <typename Bus>
concept TmcBus = kb::iface::SpiDevice<Bus> || kb::iface::UartPort<Bus>;

/** @brief The port type and bus kind for `Bus` (a SPI device wins when a type is both). */
template <typename Bus, bool kControlPins>
struct PortSelect {};

/** @brief SPI. */
template <typename Bus, bool kControlPins>
  requires kb::iface::SpiDevice<Bus>
struct PortSelect<Bus, kControlPins> {
  using Type = SpiPort<Bus, kControlPins>;       ///< The port.
  static constexpr BusKind kKind = BusKind::Spi; ///< The bus kind.
};

/** @brief UART. */
template <typename Bus, bool kControlPins>
  requires(kb::iface::UartPort<Bus> && (!kb::iface::SpiDevice<Bus>))
struct PortSelect<Bus, kControlPins> {
  using Type = UartPort<Bus, kControlPins>;       ///< The port.
  static constexpr BusKind kKind = BusKind::Uart; ///< The bus kind.
};

/** @brief The port a handler over `Bus` uses. */
template <TmcBus Bus, bool kControlPins = false>
using PortOf = typename PortSelect<Bus, kControlPins>::Type;

/** @brief The driver type a handler over `Bus` owns. */
template <TmcBus Bus, bool kControlPins = false>
using Driver = ::tmc9660::TMC9660<PortOf<Bus, kControlPins>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::tmc9660::Config chip{}; ///< Mode, addresses, timeouts, reset policy.
  HandlerConfig handler{};  ///< Health, recovery, polling.
  kb::Milliseconds start_deadline{
      1000U};         ///< Bound of the start-up stage (above `chip.boot_timeout_ms`).
  ControlPins pins{}; ///< Control pins (used with `kControlPins`).
};

/** @brief What a sample publishes. */
struct Snapshot {
  int64_t velocity{0};         ///< ACTUAL_VELOCITY (parameter mode; 0 otherwise).
  int64_t position{0};         ///< ACTUAL_POSITION (parameter mode; 0 otherwise).
  uint32_t general_errors{0U}; ///< GENERAL_ERROR_FLAGS (parameter mode; 0 otherwise).
  ::tmc9660::Mode mode{::tmc9660::Mode::Parameter}; ///< The protocol the chip is in.
  uint8_t last_status{0U};                          ///< The status byte of the chip's last reply.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  DisableDrive = 0,      ///< Switch the gate driver off (never refused).
  EnableDrive = 1,       ///< Energise in parameter mode with COMMUTATION_MODE `small`.
  MotorStop = 2,         ///< Stop the motor (MST).
  SetTargetVelocity = 3, ///< TARGET_VELOCITY to `value`.
  SetTargetTorque = 4,   ///< TARGET_TORQUE to `value`.
  SetTargetPosition = 5, ///< TARGET_POSITION to `value`.
  GetParam = 6,          ///< `param` (reply `value`).
  SetParam = 7,          ///< `param` to `value`, checked against the table.
  ClearFlags = 8,        ///< Write 1 to `mask` bits of flag parameter `param`.
  ReadReg = 9,           ///< Register `reg` (register mode; reply `word`).
  WriteReg = 10,         ///< Register `reg` to `mask` (register mode; energising bits refused).
  GetChipId = 11,        ///< GetInfo chip ID (parameter mode; reply `word`).
  Service = 12,          ///< FAULTN and the error flags now (heartbeat).
};

/** @brief One command. */
struct Command {
  int64_t value{0};                                  ///< Target or parameter value.
  uint32_t mask{0U};                                 ///< Flag bits, or the register value.
  ::tmc9660::RegAddr reg{::tmc9660::Block::Mcc, 0U}; ///< For ReadReg, WriteReg.
  ::tmc9660::Param param{
      ::tmc9660::Param::ACTUAL_VELOCITY}; ///< For GetParam, SetParam, ClearFlags.
  Op op{Op::Service};                     ///< What to do.
  uint8_t small{0U};                      ///< EnableDrive: COMMUTATION_MODE 1 to 8.
};

/** @brief The reply of a command. */
struct Reply {
  int64_t value{0};  ///< GetParam.
  uint32_t word{0U}; ///< ReadReg, GetChipId.
};

/**
 * @brief The tmc9660 handler.
 * @tparam Bus The bus: a SpiDevice (SPI) or a UartPort (UART), or their *Ref.
 * @tparam kControlPins True: the driver drives RSTN, DRV_EN, WAKE and reads FAULTN through
 * `Config::pins`.
 */
template <TmcBus Bus, bool kControlPins = false>
class Handler : public HandlerCore<Handler<Bus, kControlPins>, Driver<Bus, kControlPins>, Snapshot,
                                   Command, Reply> {
  using Core =
      HandlerCore<Handler<Bus, kControlPins>, Driver<Bus, kControlPins>, Snapshot, Command, Reply>;
  using Drv = Driver<Bus, kControlPins>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false;                         ///< Occasional calls.
  static constexpr BusKind kBus = PortSelect<Bus, kControlPins>::kKind; ///< SPI or UART.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing, config.pins), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Drv>& slot) noexcept {
    slot.emplace(port_, config_.chip);
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
    return (detail == DetailOf(::tmc9660::Error::UnexpectedReset)) ||
           (detail == DetailOf(::tmc9660::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Drv& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<void> service = LiftStatus(driver.Service(), &ToStatus);
    return service.IsOk() ? sample(driver) : Forward<Snapshot>(service);
  }
  [[nodiscard]] kb::Result<Reply> Run(Drv& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::DisableDrive:
      result = done(LiftStatus(driver.DisableDrive(), &ToStatus));
      break;
    case Op::EnableDrive:
      result = done(LiftStatus(driver.EnableDrive(c.small), &ToStatus));
      break;
    case Op::MotorStop:
      result = done(LiftStatus(driver.MotorStop(), &ToStatus));
      break;
    case Op::SetTargetVelocity:
    case Op::SetTargetTorque:
    case Op::SetTargetPosition:
      result = target(driver, c);
      break;
    case Op::GetParam:
      result = value(Lift(driver.GetParam(c.param), &ToStatus));
      break;
    case Op::SetParam:
      result = done(LiftStatus(driver.SetParam(c.param, c.value), &ToStatus));
      break;
    case Op::ClearFlags:
      result = done(LiftStatus(driver.ClearFlags(c.param, c.mask), &ToStatus));
      break;
    case Op::ReadReg:
      result = word(Lift(driver.ReadReg(c.reg), &ToStatus));
      break;
    case Op::WriteReg:
      result = done(LiftStatus(driver.WriteReg(c.reg, c.mask), &ToStatus));
      break;
    case Op::GetChipId:
      result = word(Lift(driver.GetChipId(), &ToStatus));
      break;
    case Op::Service:
      result = done(LiftStatus(driver.Service(), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's bus transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  // The values of parameter mode after a good Service(); the mode and status alone otherwise.
  [[nodiscard]] static kb::Result<Snapshot> sample(Drv& driver) noexcept {
    Snapshot s{};
    s.mode = driver.GetMode();
    kb::Result<int64_t> flags = kb::Result<int64_t>::Success(0);
    if (s.mode == ::tmc9660::Mode::Parameter) {
      const kb::Result<int64_t> v =
          Lift(driver.GetParam(::tmc9660::Param::ACTUAL_VELOCITY), &ToStatus);
      const kb::Result<int64_t> p =
          v.IsOk() ? Lift(driver.GetParam(::tmc9660::Param::ACTUAL_POSITION), &ToStatus) : v;
      flags =
          p.IsOk() ? Lift(driver.GetParam(::tmc9660::Param::GENERAL_ERROR_FLAGS), &ToStatus) : p;
      s.velocity = v.ValueOr(0);
      s.position = p.ValueOr(0);
      s.general_errors = static_cast<uint32_t>(flags.ValueOr(0));
    }
    s.last_status = driver.LastStatus();
    return flags.IsOk() ? kb::Result<Snapshot>::Success(s) : Forward<Snapshot>(flags);
  }
  [[nodiscard]] static kb::Result<Reply> target(Drv& driver, const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::OutOfRange);
    const bool fits = (c.value >= static_cast<int64_t>(std::numeric_limits<int32_t>::min())) &&
                      (c.value <= static_cast<int64_t>(std::numeric_limits<int32_t>::max()));
    if (fits) {
      const int32_t v = static_cast<int32_t>(c.value);
      if (c.op == Op::SetTargetVelocity) {
        result = done(LiftStatus(driver.SetTargetVelocity(v), &ToStatus));
      } else if (c.op == Op::SetTargetTorque) {
        result = done(LiftStatus(driver.SetTargetTorque(v), &ToStatus));
      } else {
        result = done(LiftStatus(driver.SetTargetPosition(v), &ToStatus));
      }
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> value(const kb::Result<int64_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> word(const kb::Result<uint32_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.word = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  PortOf<Bus, kControlPins> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Runs `c` and drops the reply value. */
template <typename Access>
[[nodiscard]] kb::Result<void> Send(Access& io, const Command& c,
                                    kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Energise the gate driver with COMMUTATION_MODE `commutation_mode` (1 to 8). */
template <typename Access>
[[nodiscard]] kb::Result<void> EnableDrive(Access& io, uint8_t commutation_mode,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::EnableDrive;
  c.small = commutation_mode;
  return Send(io, c, timeout);
}

/** @brief Switch the gate driver off. */
template <typename Access>
[[nodiscard]] kb::Result<void> DisableDrive(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisableDrive;
  return Send(io, c, timeout);
}

/** @brief TARGET_VELOCITY. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetTargetVelocity(Access& io, int32_t velocity,
                                                 kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetTargetVelocity;
  c.value = velocity;
  return Send(io, c, timeout);
}

/** @brief TARGET_POSITION. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetTargetPosition(Access& io, int32_t position,
                                                 kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetTargetPosition;
  c.value = position;
  return Send(io, c, timeout);
}

/** @brief An axis parameter, read now. */
template <typename Access>
[[nodiscard]] kb::Result<int64_t> GetParam(Access& io, ::tmc9660::Param param,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::GetParam;
  c.param = param;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int64_t>::Success(r.Value().value) : Forward<int64_t>(r);
}

/** @brief An axis parameter, written after the driver's table checks. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetParam(Access& io, ::tmc9660::Param param, int64_t value,
                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetParam;
  c.param = param;
  c.value = value;
  return Send(io, c, timeout);
}

/// @}

} // namespace kb::handlers::tmc9660

#endif // KB_HANDLERS_BINDINGS_TMC9660_HPP
