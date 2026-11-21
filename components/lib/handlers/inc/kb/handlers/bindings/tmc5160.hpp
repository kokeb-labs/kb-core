/**
 * @file tmc5160.hpp
 * @brief The tmc5160 binding and handler: TMC5160 / TMC5160A stepper motion controller and driver,
 * on SPI or on the single-wire UART.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `PortOf<Bus, kControlPins>`: `SpiPort` over a kb::iface::SpiDevice (SpiHooks) or
 * `UartPort` over a kb::iface::UartPort (UartHooks: flush, write, read the reply), `DelayUs`
 * through the hook timing; with `kControlPins` also `GpioSet`/`GpioGet` over GpioHooks (DRV_ENN,
 * DCEN, DCIN; DIAG0, DIAG1, DCO) | | Errors | `ToStatus()`: every `tmc5160::Error` to a kb::Status;
 * the code in the health record's detail | | Start-up | `Identify + Configure` =
 * `driver.EnsureInitialized()` (sync): bus up, quiesce, IOIN version and SD_MODE strap, GSTAT
 * cleared, every register written | | Thread model | Guarded (`kRequiresOwner = false`): motion
 * commands are sporadic and the ramp runs in the chip; an owner bus works too and samples the
 * status every `sample_period` | | Health | `UnexpectedReset` (the chip lost its registers) and
 * `NotInitialized` fault at once; other failures count to the threshold | | App API | snapshot
 * `{status, enabled, fault}` (`ReadSnapshot()` of the driver); commands: enable, disable, move
 * to/by, velocity, stop, current, position and event reads |
 *
 * The bus type picks the transport: a kb::iface::SpiDevice is the SPI datagram link, a
 * kb::iface::UartPort the UART link (the port must not return the echo of its own bytes).
 * `kControlPins = false` (the default) leaves the control pins to the board: the driver then
 * switches the bridge with TOFF only and DRV_ENN must be tied low. With `kControlPins = true` the
 * pins come from `Config::pins` (index `kPinDrvEnn`... for outputs, `kPinDiag0`... for inputs);
 * an output or input the board does not wire then fails its hook (`Bus` from the driver).
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Guarded> spi1{dev};
 * kb::handlers::Guarded<kb::handlers::tmc5160::Handler<Dev>, decltype(spi1)> axis{spi1, dev,
 * timing, cfg}; if (axis.EnsureStarted(kb::Milliseconds{100U}).IsOk()) {
 *   static_cast<void>(kb::handlers::tmc5160::Enable(axis, kb::Milliseconds{10U}));
 *   static_cast<void>(kb::handlers::tmc5160::MoveTo(axis, 51200, kb::Milliseconds{10U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_TMC5160_HPP
#define KB_HANDLERS_BINDINGS_TMC5160_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <tmc5160.hpp>

namespace kb::handlers::tmc5160 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::tmc5160::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::tmc5160::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::tmc5160::Error::NotInitialized:
  case ::tmc5160::Error::UnexpectedReset:
    s = kb::Status::NotReady;
    break;
  case ::tmc5160::Error::InvalidArgument:
  case ::tmc5160::Error::Refused:
    s = kb::Status::InvalidArgument;
    break;
  case ::tmc5160::Error::LimitExceeded:
    s = kb::Status::OutOfRange;
    break;
  case ::tmc5160::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::tmc5160::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::tmc5160::Error::DeviceIdMismatch:
    s = kb::Status::NoDevice;
    break;
  case ::tmc5160::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::tmc5160::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::tmc5160::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::tmc5160::Error::DeviceFault:
  case ::tmc5160::Error::VerifyFailed:
  case ::tmc5160::Error::ModeMismatch:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::tmc5160::Error error) noexcept {
  return (error == ::tmc5160::Error::UnexpectedReset) ||
         (error == ::tmc5160::Error::NotInitialized);
}

/// @name Control pin indices of `ControlPins` (the driver's `CtrlPin` and `InputPin` order)
/// @{
inline constexpr std::size_t kPinDrvEnn = 0U;   ///< Output: DRV_ENN (asserted = driver enabled).
inline constexpr std::size_t kPinDcEnable = 1U; ///< Output: ENCB_DCEN_CFG4.
inline constexpr std::size_t kPinDcIn = 2U;     ///< Output: ENCA_DCIN_CFG5.
inline constexpr std::size_t kPinDiag0 = 0U;    ///< Input: DIAG0 (asserted = active).
inline constexpr std::size_t kPinDiag1 = 1U;    ///< Input: DIAG1.
inline constexpr std::size_t kPinDcOut = 2U;    ///< Input: ENCN_DCO_CFG6.
inline constexpr std::size_t kOutputs = 3U;     ///< Control outputs.
inline constexpr std::size_t kInputs = 3U;      ///< Control inputs.
/// @}

/** @brief The control pins a HAL may supply (board values: pins and polarity). */
struct ControlPins {
  std::array<OutputPin, kOutputs> outputs{}; ///< DRV_ENN (usually `active_low`), DCEN, DCIN.
  std::array<InputPin, kInputs> inputs{}; ///< DIAG0, DIAG1 (open drain: usually `active_low`), DCO.
};

/** @brief The driver's microsecond timeout as whole milliseconds, rounded up. */
[[nodiscard]] constexpr uint32_t CeilMs(uint32_t us) noexcept {
  return (us / 1000U) + (((us % 1000U) != 0U) ? 1U : 0U);
}

/** @brief The output index of a driver control pin. */
[[nodiscard]] constexpr std::size_t OutputIndex(::tmc5160::CtrlPin pin) noexcept {
  std::size_t index = kPinDrvEnn;
  switch (pin) {
  case ::tmc5160::CtrlPin::DrvEnn:
    index = kPinDrvEnn;
    break;
  case ::tmc5160::CtrlPin::DcEnable:
    index = kPinDcEnable;
    break;
  case ::tmc5160::CtrlPin::DcIn:
    index = kPinDcIn;
    break;
  }
  return index;
}

/** @brief The input index of a driver input pin. */
[[nodiscard]] constexpr std::size_t InputIndex(::tmc5160::InputPin pin) noexcept {
  std::size_t index = kPinDiag0;
  switch (pin) {
  case ::tmc5160::InputPin::Diag0:
    index = kPinDiag0;
    break;
  case ::tmc5160::InputPin::Diag1:
    index = kPinDiag1;
    break;
  case ::tmc5160::InputPin::DcOut:
    index = kPinDcOut;
    break;
  }
  return index;
}

/**
 * @brief The driver's SPI port over a kb::iface::SpiDevice (one chip select, mode 3).
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 * @tparam kControlPins True: the GpioSet/GpioGet hooks exist and use `ControlPins`.
 */
template <kb::iface::SpiDevice Dev, bool kControlPins>
class SpiPort : public ::tmc5160::SpiInterface<SpiPort<Dev, kControlPins>>,
                public SpiHooks<SpiPort<Dev, kControlPins>, Dev>,
                public GpioHooks<SpiPort<Dev, kControlPins>, kOutputs, kInputs> {
  using Bus = SpiHooks<SpiPort<Dev, kControlPins>, Dev>;
  using Pins = GpioHooks<SpiPort<Dev, kControlPins>, kOutputs, kInputs>;

public:
  /** @brief Over `dev` with `timing` and the board's `pins`; no I/O. */
  SpiPort(Dev& dev, const HookTiming& timing, const ControlPins& pins) noexcept
      : Bus(dev, timing), Pins(pins.outputs, pins.inputs) {}

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
  /** @brief Optional hook: drive a control pin (with `kControlPins`). */
  bool GpioSet(::tmc5160::CtrlPin pin, ::tmc5160::GpioSignal signal) noexcept
    requires(kControlPins)
  {
    return this->DriveOutput(OutputIndex(pin), signal == ::tmc5160::GpioSignal::ACTIVE);
  }
  /** @brief Optional hook: read an input pin, `level` true when asserted (with `kControlPins`). */
  bool GpioGet(::tmc5160::InputPin pin, bool& level) noexcept
    requires(kControlPins)
  {
    return this->ReadInput(InputIndex(pin), level);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Bus::Counters();
  }
};

/**
 * @brief The driver's single-wire UART port over a kb::iface::UartPort.
 * @tparam Uart The port (a UartPort or UartPortRef); it must not return the echo of its own bytes.
 * @tparam kControlPins True: the GpioSet/GpioGet hooks exist and use `ControlPins`.
 */
template <kb::iface::UartPort Uart, bool kControlPins>
class UartPort : public ::tmc5160::UartInterface<UartPort<Uart, kControlPins>>,
                 public UartHooks<UartPort<Uart, kControlPins>, Uart>,
                 public GpioHooks<UartPort<Uart, kControlPins>, kOutputs, kInputs> {
  using Bus = UartHooks<UartPort<Uart, kControlPins>, Uart>;
  using Pins = GpioHooks<UartPort<Uart, kControlPins>, kOutputs, kInputs>;

public:
  /** @brief Over `uart` with `timing` and the board's `pins`; no I/O. */
  UartPort(Uart& uart, const HookTiming& timing, const ControlPins& pins) noexcept
      : Bus(uart, timing), Pins(pins.outputs, pins.inputs) {}

  /** @brief Hook: bring the port up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: drop stale bytes, send `tx`, then receive exactly `rx_len` bytes. */
  ::tmc5160::BusStatus Exchange(const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                                std::size_t rx_len, uint32_t timeout_us) noexcept {
    ::tmc5160::BusStatus status = ::tmc5160::BusStatus::Fault;
    if (this->BusFlush() && this->BusWrite(tx, tx_len)) {
      const bool all = this->BusRead(rx, rx_len, CeilMs(timeout_us)) == rx_len;
      status = all ? ::tmc5160::BusStatus::Ok : ::tmc5160::BusStatus::Timeout;
    }
    return status;
  }
  /** @brief Hook: wait at least `us` microseconds. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: drive a control pin (with `kControlPins`). */
  bool GpioSet(::tmc5160::CtrlPin pin, ::tmc5160::GpioSignal signal) noexcept
    requires(kControlPins)
  {
    return this->DriveOutput(OutputIndex(pin), signal == ::tmc5160::GpioSignal::ACTIVE);
  }
  /** @brief Optional hook: read an input pin, `level` true when asserted (with `kControlPins`). */
  bool GpioGet(::tmc5160::InputPin pin, bool& level) noexcept
    requires(kControlPins)
  {
    return this->ReadInput(InputIndex(pin), level);
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
using Driver = ::tmc5160::TMC5160<PortOf<Bus, kControlPins>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::tmc5160::Config chip{};              ///< What EnsureInitialized() writes (motor, board, ramp).
  HandlerConfig handler{};               ///< Health, recovery, polling.
  kb::Milliseconds start_deadline{200U}; ///< Bound of the start-up stage.
  ControlPins pins{};                    ///< Control pins (used with `kControlPins`).
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::tmc5160::Snapshot status{}; ///< GSTAT, DRV_STATUS, RAMP_STAT, XACTUAL, VACTUAL, TSTEP, MSCNT.
  bool enabled{false};          ///< The driver stage is enabled (as the driver last set it).
  bool fault{false};            ///< The driver reported an error (GSTAT.drv_err) since cleared.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  Enable = 0,           ///< Enable the driver stage (TOFF, DRV_ENN).
  Disable = 1,          ///< Disable the driver stage.
  MoveTo = 2,           ///< Position mode to `value` (microsteps).
  MoveBy = 3,           ///< Position mode by `value` microsteps.
  RunVelocity = 4,      ///< Velocity mode at signed `value`.
  Stop = 5,             ///< Stop as `stop` says.
  Hold = 6,             ///< Hold mode (velocity stays constant).
  SetCurrent = 7,       ///< Run and hold current `run_ma`, `hold_ma` (reply: the plan's run, hold).
  SetMaxVelocity = 8,   ///< VMAX to `value` (non-negative).
  SetHomePosition = 9,  ///< XACTUAL and XTARGET to `value` at standstill.
  ReadPosition = 10,    ///< XACTUAL (reply `value`).
  ReadVelocity = 11,    ///< VACTUAL (reply `value`).
  PositionReached = 12, ///< RAMP_STAT position reached (reply `flag`).
  ClearFaults = 13,     ///< Clear GSTAT driver errors.
  ServiceEvents = 14,   ///< Read and clear the events after a DIAG0 interrupt (reply `events`).
  ReadRegister = 15,    ///< One readable register at `address` (reply `word`).
  ReleaseStall = 16,    ///< Release a stall stop.
};

/** @brief One command. */
struct Command {
  int32_t value{0};                                    ///< Position, delta, velocity.
  uint32_t run_ma{0U};                                 ///< SetCurrent: run current, RMS mA.
  uint32_t hold_ma{0U};                                ///< SetCurrent: hold current, RMS mA.
  Op op{Op::ReadPosition};                             ///< What to do.
  ::tmc5160::StopKind stop{::tmc5160::StopKind::Ramp}; ///< For Stop.
  ::tmc5160::Verify verify{::tmc5160::Verify::None};   ///< Extra checking traffic on writes.
  uint8_t address{0U};                                 ///< For ReadRegister.
};

/** @brief The reply of a command. */
struct Reply {
  ::tmc5160::Events events{}; ///< ServiceEvents: what was found.
  int32_t value{0};           ///< ReadPosition, ReadVelocity.
  uint32_t word{0U};          ///< ReadRegister; SetCurrent: the plan's run current in mA.
  uint32_t hold_ma{0U};       ///< SetCurrent: the plan's hold current in mA.
  bool flag{false};           ///< PositionReached.
};

/**
 * @brief The tmc5160 handler.
 * @tparam Bus The bus: a SpiDevice (SPI) or a UartPort (UART), or their *Ref.
 * @tparam kControlPins True: the driver drives DRV_ENN and reads DIAG through `Config::pins`.
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
    return (detail == DetailOf(::tmc5160::Error::UnexpectedReset)) ||
           (detail == DetailOf(::tmc5160::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Drv& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<::tmc5160::Snapshot> status = Lift(driver.ReadSnapshot(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(status);
    if (status.IsOk()) {
      Snapshot s{};
      s.status = status.Value();
      s.enabled = driver.IsEnabled();
      s.fault = driver.HasFault();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Drv& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::Enable:
      result = done(LiftStatus(driver.Enable(c.verify), &ToStatus));
      break;
    case Op::Disable:
      result = done(LiftStatus(driver.Disable(c.verify), &ToStatus));
      break;
    case Op::MoveTo:
      result = done(LiftStatus(driver.MoveTo(c.value, c.verify), &ToStatus));
      break;
    case Op::MoveBy:
      result = done(LiftStatus(driver.MoveBy(c.value, c.verify), &ToStatus));
      break;
    case Op::RunVelocity:
      result = done(LiftStatus(driver.RunVelocity(c.value, c.verify), &ToStatus));
      break;
    case Op::Stop:
      result = done(LiftStatus(driver.Stop(c.stop, c.verify), &ToStatus));
      break;
    case Op::Hold:
      result = done(LiftStatus(driver.Hold(c.verify), &ToStatus));
      break;
    case Op::SetCurrent:
      result = plan(Lift(driver.SetCurrent(c.run_ma, c.hold_ma, c.verify), &ToStatus));
      break;
    case Op::SetMaxVelocity:
      result = setVmax(driver, c);
      break;
    case Op::SetHomePosition:
      result = done(LiftStatus(driver.SetHomePosition(c.value), &ToStatus));
      break;
    case Op::ReadPosition:
      result = signedValue(Lift(driver.ReadPosition(), &ToStatus));
      break;
    case Op::ReadVelocity:
      result = signedValue(Lift(driver.ReadVelocity(), &ToStatus));
      break;
    case Op::PositionReached:
      result = flag(Lift(driver.PositionReached(), &ToStatus));
      break;
    case Op::ClearFaults:
      result = done(LiftStatus(driver.ClearFaults(), &ToStatus));
      break;
    case Op::ServiceEvents:
      result = events(Lift(driver.Service(), &ToStatus));
      break;
    case Op::ReadRegister:
      result = word(Lift(driver.ReadRegister(c.address), &ToStatus));
      break;
    case Op::ReleaseStall:
      result = done(LiftStatus(driver.ReleaseStall(), &ToStatus));
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
  [[nodiscard]] static kb::Result<Reply> setVmax(Drv& driver, const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    if (c.value >= 0) {
      result = done(
          LiftStatus(driver.SetMaxVelocity(static_cast<uint32_t>(c.value), c.verify), &ToStatus));
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> signedValue(const kb::Result<int32_t>& r) noexcept {
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
  [[nodiscard]] static kb::Result<Reply> flag(const kb::Result<bool>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.flag = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> plan(
      const kb::Result<::tmc5160::units::CurrentPlan>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.word = r.Value().run_ma;
      reply.hold_ma = r.Value().hold_ma;
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> events(const kb::Result<::tmc5160::Events>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.events = r.Value();
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

/** @brief Enable the driver stage. */
template <typename Access>
[[nodiscard]] kb::Result<void> Enable(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Enable;
  return Send(io, c, timeout);
}

/** @brief Disable the driver stage. */
template <typename Access>
[[nodiscard]] kb::Result<void> Disable(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Disable;
  return Send(io, c, timeout);
}

/** @brief Move to `position` (microsteps) with the configured ramp. */
template <typename Access>
[[nodiscard]] kb::Result<void> MoveTo(Access& io, int32_t position,
                                      kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::MoveTo;
  c.value = position;
  return Send(io, c, timeout);
}

/** @brief Run at the signed `velocity` (velocity mode). */
template <typename Access>
[[nodiscard]] kb::Result<void> RunVelocity(Access& io, int32_t velocity,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::RunVelocity;
  c.value = velocity;
  return Send(io, c, timeout);
}

/** @brief Stop as `kind` says. */
template <typename Access>
[[nodiscard]] kb::Result<void> Stop(Access& io, ::tmc5160::StopKind kind,
                                    kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Stop;
  c.stop = kind;
  return Send(io, c, timeout);
}

/** @brief Set the run and hold current (RMS mA); returns the run current the registers give. */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> SetCurrent(Access& io, uint32_t run_ma, uint32_t hold_ma,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetCurrent;
  c.run_ma = run_ma;
  c.hold_ma = hold_ma;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().word) : Forward<uint32_t>(r);
}

/** @brief XACTUAL, read now. */
template <typename Access>
[[nodiscard]] kb::Result<int32_t> ReadPosition(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadPosition;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int32_t>::Success(r.Value().value) : Forward<int32_t>(r);
}

/** @brief Read and clear the events (after DIAG0). */
template <typename Access>
[[nodiscard]] kb::Result<::tmc5160::Events> ServiceEvents(Access& io,
                                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ServiceEvents;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::tmc5160::Events>::Success(r.Value().events)
                  : Forward<::tmc5160::Events>(r);
}

/// @}

} // namespace kb::handlers::tmc5160

#endif // KB_HANDLERS_BINDINGS_TMC5160_HPP
