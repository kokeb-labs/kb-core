/**
 * @file bno08x.hpp
 * @brief The bno08x binding and handler: BNO080 / BNO085 / BNO086 sensor hub, SHTP over I2C, SPI
 * or UART (ADR-0009, ADR-0010).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `PortOf<Bus, kReset>`: `I2cPort` over a kb::iface::I2cBus, `SpiPort` over a
 * kb::iface::SpiDevice, `UartPort` (UART-SHTP) over a kb::iface::UartPort; `NowUs` from the hook
 * timing; H_INTN, WAKE and NRST over GpioHooks (`Config::pins`); with `kReset` the `SetReset` hook
 * (NRST) exists | | Errors | `ToStatus()`: every `bno08x::Error` to a kb::Status; the code in the
 * health record's detail and in each operation record | | Start-up | `HostReset` = `Begin(cfg)`
 * (sync; `Link` without NRST); `Boot + Link` = `Poll()` until `Ready()` (polled, from
 * `startup_timeout_ms`; `Identify` without NRST), fails when the driver enters `Faulted`;
 * `Configure` = one `SetFeature` per configured sensor, each polled until its `OpDone` (from
 * `op_timeout_ms`) | | Thread model | Owner only (`kRequiresOwner = true`): the hub streams reports
 * and completes every command asynchronously, so it must be polled continuously; `Guarded<Handler>`
 * does not compile | | Health | the driver in `Faulted` (`NotInitialized`) faults at once and the
 * recovery runs the plan again; a hub restart (`Reset` event) records one failure (`HubReset`:
 * Degraded) while the driver restores its sensors; refused calls count to the threshold | | App API
 * | snapshot `{samples per sensor, operation records, product ID, restarts, state}`; commands: set
 * sensor, flush, tare, reorientation, reset, sleep, on, product ID, DCD, calibration; `WaitOp()`
 * waits for an operation's record |
 *
 * Each `Service()` of the owner task runs `Measure()`: one `Poll()` of the driver, then every
 * pending event goes into the snapshot (set `HandlerConfig::sample_period` 0, or at most a tenth of
 * the fastest report period). A command returns its operation number at once; the result arrives
 * later in `Snapshot::ops` (`WaitOp()`).
 *
 * Pins are given as functions, the polarity in `OutputPin`/`InputPin`: NRST asserted = in reset,
 * WAKE asserted = wake request, H_INTN asserted = the hub has data (all three are active low on the
 * package). On I2C a board without H_INTN sets `chip.poll_without_int`.
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Owner> i2c1{bus};
 * kb::handlers::OwnerAccess<kb::handlers::bno08x::Handler<Bus>, decltype(i2c1)> imu{i2c1, bus,
 * timing, cfg};
 * // the owner task: i2c1.Service(kb::rtos::Now(), imu) every millisecond
 * if (imu.EnsureStarted(kb::Milliseconds{800U}).IsOk()) {
 *   const auto rv = kb::handlers::bno08x::LatestSample(imu, ::bno08x::SensorId::RotationVector);
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_BNO08X_HPP
#define KB_HANDLERS_BINDINGS_BNO08X_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include "kb/rtos/time.hpp"
#include <bno08x.hpp>

namespace kb::handlers::bno08x {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::bno08x::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::bno08x::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::bno08x::Error::NotInitialized:
  case ::bno08x::Error::HubReset:
    s = kb::Status::NotReady;
    break;
  case ::bno08x::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::bno08x::Error::Bus:
  case ::bno08x::Error::Framing:
    s = kb::Status::BusError;
    break;
  case ::bno08x::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::bno08x::Error::DeviceIdMismatch:
    s = kb::Status::NoDevice;
    break;
  case ::bno08x::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::bno08x::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::bno08x::Error::Overflow:
    s = kb::Status::Overflow;
    break;
  case ::bno08x::Error::Oversize:
    s = kb::Status::OutOfRange;
    break;
  case ::bno08x::Error::Unsupported:
  case ::bno08x::Error::NotAdvertised:
  case ::bno08x::Error::Variant:
    s = kb::Status::Unsupported;
    break;
  case ::bno08x::Error::DeviceFault:
  case ::bno08x::Error::Refused:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/// @name Control pin indices of `ControlPins`
/// @{
inline constexpr std::size_t kPinNrst = 0U; ///< Output: NRST (asserted = in reset).
inline constexpr std::size_t kPinWake = 1U; ///< Output: PS0/WAKE (SPI; asserted = wake request).
inline constexpr std::size_t kPinInt = 0U;  ///< Input: H_INTN (asserted = the hub has data).
inline constexpr std::size_t kOutputs = 2U; ///< Control outputs.
inline constexpr std::size_t kInputs = 1U;  ///< Control inputs.
/// @}

/** @brief The control pins a HAL may supply (board values: pins and polarity). */
struct ControlPins {
  std::array<OutputPin, kOutputs> outputs{}; ///< NRST, WAKE (both usually `active_low`).
  std::array<InputPin, kInputs> inputs{};    ///< H_INTN (usually `active_low`).
};

/**
 * @brief The hooks every transport shares: the clock, H_INTN and NRST over GpioHooks.
 * @tparam Derived The port.
 */
template <typename Derived>
class HubPins : public GpioHooks<Derived, kOutputs, kInputs> {
  using Pins = GpioHooks<Derived, kOutputs, kInputs>;

protected:
  /** @brief Over the board's `pins`; no I/O. */
  explicit HubPins(const ControlPins& pins) noexcept : Pins(pins.outputs, pins.inputs) {}

  /** @brief H_INTN asserted; false when the board does not wire it. */
  [[nodiscard]] bool IntLine() noexcept {
    bool asserted = false;
    if (this->HasInput(kPinInt)) {
      static_cast<void>(this->ReadInput(kPinInt, asserted));
    }
    return asserted;
  }
  /** @brief NRST to `level` (true = high = released). */
  [[nodiscard]] bool ResetLine(bool level) noexcept {
    return this->DriveOutput(kPinNrst, !level);
  }
  /** @brief WAKE to `level` (true = high = idle). */
  [[nodiscard]] bool WakeLine(bool level) noexcept {
    return this->DriveOutput(kPinWake, !level);
  }
};

/**
 * @brief The driver's I2C port (SHTP over I2C) over a kb::iface::I2cBus.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 * @tparam kReset True: the `SetReset` hook (NRST) exists.
 */
template <kb::iface::I2cBus Bus, bool kReset>
class I2cPort : public ::bno08x::HubInterface<I2cPort<Bus, kReset>>,
                public I2cHooks<I2cPort<Bus, kReset>, Bus>,
                public HubPins<I2cPort<Bus, kReset>> {
  using Hooks = I2cHooks<I2cPort<Bus, kReset>, Bus>;
  using Pins = HubPins<I2cPort<Bus, kReset>>;

public:
  static constexpr ::bno08x::Transport kTransport = ::bno08x::Transport::I2c; ///< SHTP over I2C.

  /** @brief Over `bus` with `timing` and the board's `pins`; no I/O. */
  I2cPort(Bus& bus, const HookTiming& timing, const ControlPins& pins) noexcept
      : Hooks(bus, timing), Pins(pins) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: the free-running microsecond clock. */
  uint32_t NowUs() noexcept {
    return this->Timing().NowUs32();
  }
  /** @brief Hook: one write transfer. */
  bool I2cWrite(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: one read transfer (never a repeated start). */
  bool I2cRead(uint8_t addr, uint8_t* data, std::size_t len) noexcept {
    return this->BusRead(addr, data, len);
  }
  /** @brief Hook: H_INTN asserted. */
  bool IntAsserted() noexcept {
    return this->IntLine();
  }
  /** @brief Optional hook: NRST level (with `kReset`). */
  bool SetReset(bool level) noexcept
    requires(kReset)
  {
    return this->ResetLine(level);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Hooks::Counters();
  }
};

/**
 * @brief The driver's SPI port (SHTP over SPI, with H_INTN and WAKE) over a kb::iface::SpiDevice.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef; mode 3, at most 3 MHz).
 * @tparam kReset True: the `SetReset` hook (NRST) exists.
 */
template <kb::iface::SpiDevice Dev, bool kReset>
class SpiPort : public ::bno08x::HubInterface<SpiPort<Dev, kReset>>,
                public SpiHooks<SpiPort<Dev, kReset>, Dev>,
                public HubPins<SpiPort<Dev, kReset>> {
  using Hooks = SpiHooks<SpiPort<Dev, kReset>, Dev>;
  using Pins = HubPins<SpiPort<Dev, kReset>>;

public:
  static constexpr ::bno08x::Transport kTransport = ::bno08x::Transport::Spi; ///< SHTP over SPI.

  /** @brief Over `dev` with `timing` and the board's `pins`; no I/O. */
  SpiPort(Dev& dev, const HookTiming& timing, const ControlPins& pins) noexcept
      : Hooks(dev, timing), Pins(pins) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: the free-running microsecond clock. */
  uint32_t NowUs() noexcept {
    return this->Timing().NowUs32();
  }
  /** @brief Hook: one full-duplex transfer under one chip select. */
  bool SpiTransfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return this->BusTransfer(tx, rx, len);
  }
  /** @brief Hook: PS0/WAKE level (true = high = idle). */
  bool SetWake(bool level) noexcept {
    return this->WakeLine(level);
  }
  /** @brief Hook: H_INTN asserted. */
  bool IntAsserted() noexcept {
    return this->IntLine();
  }
  /** @brief Optional hook: NRST level (with `kReset`). */
  bool SetReset(bool level) noexcept
    requires(kReset)
  {
    return this->ResetLine(level);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Hooks::Counters();
  }
};

/**
 * @brief The driver's UART-SHTP port over a kb::iface::UartPort (the driver paces the bytes).
 * @tparam Uart The port (a UartPort or UartPortRef; 3 Mbit/s).
 * @tparam kReset True: the `SetReset` hook (NRST) exists.
 */
template <kb::iface::UartPort Uart, bool kReset>
class UartPort : public ::bno08x::HubInterface<UartPort<Uart, kReset>>,
                 public UartHooks<UartPort<Uart, kReset>, Uart>,
                 public HubPins<UartPort<Uart, kReset>> {
  using Hooks = UartHooks<UartPort<Uart, kReset>, Uart>;
  using Pins = HubPins<UartPort<Uart, kReset>>;

public:
  static constexpr ::bno08x::Transport kTransport = ::bno08x::Transport::UartShtp; ///< UART-SHTP.

  /** @brief Over `uart` with `timing` and the board's `pins`; no I/O. */
  UartPort(Uart& uart, const HookTiming& timing, const ControlPins& pins) noexcept
      : Hooks(uart, timing), Pins(pins) {}

  /** @brief Hook: bring the port up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: the free-running microsecond clock. */
  uint32_t NowUs() noexcept {
    return this->Timing().NowUs32();
  }
  /** @brief Hook: the bytes received now, at most `cap`; never waits. */
  std::size_t UartRead(uint8_t* data, std::size_t cap) noexcept {
    return this->BusRead(data, cap, 0U);
  }
  /** @brief Hook: queue one byte. */
  bool UartWriteByte(uint8_t b) noexcept {
    const std::array<uint8_t, 1> one{b};
    return this->BusWrite(one.data(), one.size());
  }
  /** @brief Optional hook: NRST level (with `kReset`). */
  bool SetReset(bool level) noexcept
    requires(kReset)
  {
    return this->ResetLine(level);
  }
  /** @brief The bus transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Hooks::Counters();
  }
};

/** @brief A bus the binding can serve: I2C, SPI or UART. */
template <typename Bus>
concept HubBus = kb::iface::I2cBus<Bus> || kb::iface::SpiDevice<Bus> || kb::iface::UartPort<Bus>;

/** @brief The port type and bus kind for `Bus` (I2C, then SPI, then UART when a type is several).
 */
template <typename Bus, bool kReset>
struct PortSelect {};

/** @brief I2C. */
template <typename Bus, bool kReset>
  requires kb::iface::I2cBus<Bus>
struct PortSelect<Bus, kReset> {
  using Type = I2cPort<Bus, kReset>;             ///< The port.
  static constexpr BusKind kKind = BusKind::I2c; ///< The bus kind.
};

/** @brief SPI. */
template <typename Bus, bool kReset>
  requires(kb::iface::SpiDevice<Bus> && (!kb::iface::I2cBus<Bus>))
struct PortSelect<Bus, kReset> {
  using Type = SpiPort<Bus, kReset>;             ///< The port.
  static constexpr BusKind kKind = BusKind::Spi; ///< The bus kind.
};

/** @brief UART-SHTP. */
template <typename Bus, bool kReset>
  requires(kb::iface::UartPort<Bus> && (!kb::iface::I2cBus<Bus>) && (!kb::iface::SpiDevice<Bus>))
struct PortSelect<Bus, kReset> {
  using Type = UartPort<Bus, kReset>;             ///< The port.
  static constexpr BusKind kKind = BusKind::Uart; ///< The bus kind.
};

/** @brief The port a handler over `Bus` uses. */
template <HubBus Bus, bool kReset = true>
using PortOf = typename PortSelect<Bus, kReset>::Type;

/** @brief The driver type a handler over `Bus` owns. */
template <HubBus Bus, bool kReset = true>
using Driver = ::bno08x::Bno08x<PortOf<Bus, kReset>>;

/** @brief Sensors the start-up configures, and sample slots of the snapshot. */
inline constexpr std::size_t kMaxSensors = 8U;
/** @brief Operation records the snapshot keeps (a ring, newest last written). */
inline constexpr std::size_t kOpHistory = 8U;
/** @brief Values of an operation record. */
inline constexpr std::size_t kOpValues = 4U;

/// @name Stages of the plan
/// @{
inline constexpr std::size_t kStageBegin = 0U;     ///< `Begin(cfg)`.
inline constexpr std::size_t kStageSession = 1U;   ///< `Poll()` until `Ready()`.
inline constexpr std::size_t kStageConfigure = 2U; ///< One `SetFeature` per configured sensor.
/// @}

/// The plan with NRST: the reset, the boot and SHTP link, the sensors.
inline constexpr std::array<Stage, 3> kPlanWithReset{
    {{stage::kHostReset, StageMode::Sync, false},
     {static_cast<uint16_t>(stage::kBoot | stage::kLink), StageMode::Polled, false},
     {stage::kConfigure, StageMode::Polled, false}}};

/// The plan without NRST: join the running hub (link), wait for its advertisement and initialise
/// response (identify), the sensors.
inline constexpr std::array<Stage, 3> kPlanJoin{{{stage::kLink, StageMode::Sync, false},
                                                 {stage::kIdentify, StageMode::Polled, false},
                                                 {stage::kConfigure, StageMode::Polled, false}}};

/** @brief One sensor the start-up configures. */
struct SensorSetting {
  ::bno08x::FeatureConfig feature{};                         ///< Interval, batch, flags.
  ::bno08x::SensorId id{::bno08x::SensorId::RotationVector}; ///< The sensor.
};

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::bno08x::Config chip{};                          ///< What `Begin()` takes (timeouts included).
  HandlerConfig handler{};                          ///< Health, recovery, polling.
  std::array<SensorSetting, kMaxSensors> sensors{}; ///< The sensors the start-up turns on.
  uint8_t sensor_count{0U};                         ///< How many of `sensors` are used.
  kb::Milliseconds begin_deadline{20U};             ///< Bound of the `Begin()` stage.
  kb::Milliseconds boot_margin{200U}; ///< Added to the driver's own waits in the session stage.
  ControlPins pins{};                 ///< H_INTN, NRST, WAKE.
};

/** @brief The latest sample of one sensor. */
struct Sample {
  std::array<int32_t, ::bno08x::kEventValues> v{}; ///< The report's values (its own Q point).
  uint32_t t_us{0U};                               ///< Host time of the sample (`NowUs()` domain).
  uint32_t count{0U};                              ///< Samples seen since the start.
  ::bno08x::SensorId sensor{::bno08x::SensorId::RotationVector}; ///< The sensor of this slot.
  ::bno08x::Accuracy accuracy{::bno08x::Accuracy::Unreliable};   ///< The sample's accuracy.
  bool used{false};                                              ///< The slot belongs to `sensor`.
  bool valid{false};                                             ///< A sample arrived.
};

/** @brief The outcome of one operation (`OpDone`). */
struct OpRecord {
  std::array<int32_t, kOpValues> v{};         ///< The first response values.
  uint32_t detail{0U};                        ///< The driver's kind-specific word.
  ::bno08x::OpId op{0U};                      ///< The operation number.
  ::bno08x::Error error{::bno08x::Error::Ok}; ///< Its outcome.
  bool valid{false};                          ///< The record is filled.
};

/** @brief What a sample publishes. */
struct Snapshot {
  std::array<Sample, kMaxSensors> samples{};    ///< One slot per sensor (configured first).
  std::array<OpRecord, kOpHistory> ops{};       ///< The latest operation outcomes.
  ::bno08x::ProductId product{};                ///< Entry 0 of the last product ID request.
  uint32_t restarts{0U};                        ///< Hub restarts seen since the start.
  uint32_t dropped{0U};                         ///< Events the driver dropped (full queue).
  uint32_t hub_errors{0U};                      ///< SHTP and SH-2 error reports.
  ::bno08x::State state{::bno08x::State::Idle}; ///< The session state.
  uint8_t next_op{0U};                          ///< The ring slot the next record takes.
};

/** @brief The commands of the handler (each completes later with an `OpDone`). */
enum class Op : uint8_t {
  SetSensor = 0,          ///< Set Feature of `sensor` to `feature` (interval 0 turns it off).
  GetFeature = 1,         ///< Ask the hub for `sensor`'s feature report.
  Flush = 2,              ///< Flush `sensor`'s batch.
  Tare = 3,               ///< Tare `axes` on `basis`.
  PersistTare = 4,        ///< Persist the last tare.
  ClearReorientation = 5, ///< Clear the run-time reorientation.
  ResetHub = 6,           ///< Executable reset (the hub restarts; sensors are restored).
  HubSleep = 7,           ///< Executable sleep.
  HubOn = 8,              ///< Executable on.
  ReadProductId = 9,      ///< Product ID (into `Snapshot::product`).
  SaveDcd = 10,           ///< Save the dynamic calibration data.
  ConfigureMeCal = 11,    ///< Calibration enables `cal`.
};

/** @brief One command. */
struct Command {
  ::bno08x::FeatureConfig feature{};                              ///< For SetSensor.
  ::bno08x::MeCal cal{};                                          ///< For ConfigureMeCal.
  Op op{Op::ReadProductId};                                       ///< What to do.
  ::bno08x::SensorId sensor{::bno08x::SensorId::RotationVector};  ///< For the sensor commands.
  ::bno08x::AxisMask axes{::bno08x::AxisMask::All};               ///< For Tare.
  ::bno08x::TareBasis basis{::bno08x::TareBasis::RotationVector}; ///< For Tare.
};

/** @brief The reply of a command: the operation to wait for. */
struct Reply {
  ::bno08x::OpId op{0U}; ///< The operation number (`WaitOp()`).
};

/**
 * @brief The bno08x handler (owner bus only).
 * @tparam Bus The bus: an I2cBus, a SpiDevice or a UartPort (UART-SHTP), or their *Ref.
 * @tparam kReset True (the default): the board wires NRST and the driver resets the hub at the
 * start; false: the start joins the running hub.
 */
template <HubBus Bus, bool kReset = true>
class Handler
    : public HandlerCore<Handler<Bus, kReset>, Driver<Bus, kReset>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus, kReset>, Driver<Bus, kReset>, Snapshot, Command, Reply>;
  using Drv = Driver<Bus, kReset>;

public:
  /// The start-up plan of ADR-0010 for this chip (with or without NRST).
  static constexpr std::array<Stage, 3> kStartupPlan = kReset ? kPlanWithReset : kPlanJoin;
  static constexpr bool kRequiresOwner = true;                    ///< Polled continuously.
  static constexpr BusKind kBus = PortSelect<Bus, kReset>::kKind; ///< I2C, SPI or UART.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing, config.pins), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Drv>& slot) noexcept {
    slot.emplace(port_);
    clearSession();
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call, Drv& driver) noexcept {
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Failure(kb::Status::Fault);
    if (call.index == kStageBegin) {
      const kb::Result<void> r = LiftStatus(driver.Begin(config_.chip), &ToStatus);
      result = r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                        : Forward<StartProgress>(r);
    } else if (call.index == kStageSession) {
      result = session(driver);
    } else {
      result = configure(driver);
    }
    return result;
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    constexpr uint32_t kUsPerMs = 1000U;
    uint32_t ms = config_.begin_deadline.Count();
    if (index == kStageSession) {
      ms = (2U * static_cast<uint32_t>(config_.chip.startup_timeout_ms)) +
           (config_.chip.reset_pulse_us / kUsPerMs) + config_.boot_margin.Count();
    } else if (index == kStageConfigure) {
      ms = static_cast<uint32_t>(config_.chip.op_timeout_ms) *
           (static_cast<uint32_t>(config_.sensor_count) + 1U);
    } else {
      // The Begin() stage.
    }
    return kb::Milliseconds{ms};
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::bno08x::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Drv& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    pump(driver);
    kb::Result<Snapshot> result = kb::Result<Snapshot>::Success(acc_);
    if (acc_.state == ::bno08x::State::Faulted) {
      result = kb::Result<Snapshot>::Failure(kb::Status::NotReady,
                                             DetailOf(::bno08x::Error::NotInitialized));
    } else if (restart_seen_) {
      restart_seen_ = false; // one failure per restart: Degraded while the driver restores
      result = kb::Result<Snapshot>::Failure(ToStatus(::bno08x::Error::HubReset),
                                             DetailOf(::bno08x::Error::HubReset));
    } else {
      // A normal pass.
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Drv& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<::bno08x::OpId> op =
        kb::Result<::bno08x::OpId>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SetSensor:
      op = Lift(driver.SetFeature(c.sensor, c.feature), &ToStatus);
      break;
    case Op::GetFeature:
      op = Lift(driver.GetFeature(c.sensor), &ToStatus);
      break;
    case Op::Flush:
      op = Lift(driver.Flush(c.sensor), &ToStatus);
      break;
    case Op::Tare:
      op = Lift(driver.Tare(c.axes, c.basis), &ToStatus);
      break;
    case Op::PersistTare:
      op = Lift(driver.PersistTare(), &ToStatus);
      break;
    case Op::ClearReorientation:
      op = Lift(driver.ClearReorientation(), &ToStatus);
      break;
    case Op::ResetHub:
      op = Lift(driver.Reset(), &ToStatus);
      break;
    case Op::HubSleep:
      op = Lift(driver.HubSleep(), &ToStatus);
      break;
    case Op::HubOn:
      op = Lift(driver.HubOn(), &ToStatus);
      break;
    case Op::ReadProductId:
      op = Lift(driver.ReadProductId(), &ToStatus);
      break;
    case Op::SaveDcd:
      op = Lift(driver.SaveDcd(), &ToStatus);
      break;
    case Op::ConfigureMeCal:
      op = Lift(driver.ConfigureMeCal(c.cal), &ToStatus);
      break;
    }
    return reply(op);
  }
  /// @}

  /** @brief The port's bus transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  static constexpr std::size_t kDrainPerPoll = ::bno08x::DefaultLimits::kEventQueue;

  [[nodiscard]] static kb::Result<Reply> reply(const kb::Result<::bno08x::OpId>& op) noexcept {
    kb::Result<Reply> result = Forward<Reply>(op);
    if (op.IsOk()) {
      Reply r{};
      r.op = op.Value();
      result = kb::Result<Reply>::Success(r);
    }
    return result;
  }

  // A new driver: a new session, nothing seen yet; the configured sensors own the first slots.
  void clearSession() noexcept {
    acc_ = Snapshot{};
    const std::size_t count = configured();
    for (std::size_t i = 0U; i < count; ++i) {
      acc_.samples[i].sensor = config_.sensors[i].id;
      acc_.samples[i].used = true;
    }
    next_sensor_ = 0U;
    pending_op_ = 0U;
    pending_ = false;
    config_error_ = ::bno08x::Error::Ok;
    restart_seen_ = false;
  }

  [[nodiscard]] std::size_t configured() const noexcept {
    const std::size_t n = static_cast<std::size_t>(config_.sensor_count);
    return (n > kMaxSensors) ? kMaxSensors : n;
  }

  // One Poll() of the driver, then every pending event into the snapshot.
  void pump(Drv& driver) noexcept {
    // The driver's Poll() returns nothing; the cast tells the name-based Rule 0.1.2 lint that this
    // is not kb::iface::AsyncOperation::Poll(), whose result must be used.
    static_cast<void>(driver.Poll(port_.Timing().NowUs32()));
    for (std::size_t i = 0U; i < kDrainPerPoll; ++i) {
      ::bno08x::Event ev{};
      if (driver.PopEvent(ev)) {
        onEvent(ev);
      }
    }
    acc_.state = driver.GetState();
    acc_.product = driver.Product();
  }

  void onEvent(const ::bno08x::Event& ev) noexcept {
    switch (ev.kind) {
    case ::bno08x::EventKind::Sensor:
      onSample(ev);
      break;
    case ::bno08x::EventKind::OpDone:
      onOpDone(ev);
      break;
    case ::bno08x::EventKind::Reset:
      acc_.restarts = SaturatingIncrement(acc_.restarts);
      restart_seen_ = true;
      break;
    case ::bno08x::EventKind::Dropped:
      acc_.dropped += ev.detail;
      break;
    case ::bno08x::EventKind::HubError:
    case ::bno08x::EventKind::Sh2Error:
      acc_.hub_errors = SaturatingIncrement(acc_.hub_errors);
      break;
    case ::bno08x::EventKind::FeatureChanged:
    case ::bno08x::EventKind::FlushDone:
    case ::bno08x::EventKind::Rvc:
      // Not part of the snapshot (the feature shadow is in the driver; RVC is not this transport).
      break;
    }
  }

  void onSample(const ::bno08x::Event& ev) noexcept {
    std::size_t slot = kMaxSensors;
    for (std::size_t i = 0U; i < kMaxSensors; ++i) {
      const Sample& s = acc_.samples[i];
      const bool match = s.used && (s.sensor == ev.sensor);
      const bool free = (!s.used) && (slot == kMaxSensors);
      slot = (match || free) ? i : slot;
    }
    if (slot < kMaxSensors) {
      Sample& s = acc_.samples[slot];
      s.sensor = ev.sensor;
      s.used = true;
      s.valid = true;
      s.accuracy = ev.accuracy;
      s.t_us = ev.t_us;
      s.v = ev.v;
      s.count = SaturatingIncrement(s.count);
    }
  }

  void onOpDone(const ::bno08x::Event& ev) noexcept {
    OpRecord rec{};
    rec.op = ev.op;
    rec.error = ev.error;
    rec.detail = ev.detail;
    rec.valid = true;
    for (std::size_t i = 0U; i < kOpValues; ++i) {
      rec.v[i] = ev.v[i];
    }
    const std::size_t at = static_cast<std::size_t>(acc_.next_op) % kOpHistory;
    acc_.ops[at] = rec;
    acc_.next_op = static_cast<uint8_t>((at + 1U) % kOpHistory);
    if (pending_ && (ev.op == pending_op_)) {
      pending_ = false;
      config_error_ = ev.error;
    }
  }

  // Poll() until Ready(); the driver's own waits end in Faulted.
  [[nodiscard]] kb::Result<StartProgress> session(Drv& driver) noexcept {
    pump(driver);
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    if (acc_.state == ::bno08x::State::Faulted) {
      result = kb::Result<StartProgress>::Failure(kb::Status::Timeout,
                                                  DetailOf(::bno08x::Error::Timeout));
    } else if (acc_.state == ::bno08x::State::Ready) {
      result = kb::Result<StartProgress>::Success(StartProgress::Done);
    } else {
      // Still resetting, waiting for the advertisement or the initialise response.
    }
    return result;
  }

  // One SetFeature at a time, each until its OpDone.
  [[nodiscard]] kb::Result<StartProgress> configure(Drv& driver) noexcept {
    pump(driver);
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    if (config_error_ != ::bno08x::Error::Ok) {
      result = kb::Result<StartProgress>::Failure(ToStatus(config_error_), DetailOf(config_error_));
    } else if (acc_.state != ::bno08x::State::Ready) {
      result = kb::Result<StartProgress>::Failure(kb::Status::NotReady,
                                                  DetailOf(::bno08x::Error::NotInitialized));
    } else if (pending_) {
      // Waiting for this sensor's OpDone.
    } else if (next_sensor_ < configured()) {
      result = submit(driver);
    } else {
      restart_seen_ = false; // restarts before Ready belong to the start-up
      result = kb::Result<StartProgress>::Success(StartProgress::Done);
    }
    return result;
  }

  [[nodiscard]] kb::Result<StartProgress> submit(Drv& driver) noexcept {
    const SensorSetting& s = config_.sensors[next_sensor_];
    const kb::Result<::bno08x::OpId> op = Lift(driver.SetFeature(s.id, s.feature), &ToStatus);
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    if (op.IsOk()) {
      pending_op_ = op.Value();
      pending_ = true;
      ++next_sensor_;
    } else if (op.Error() != kb::Status::Busy) {
      result = Forward<StartProgress>(op);
    } else {
      // The operation queue is full: try again next step.
    }
    return result;
  }

  PortOf<Bus, kReset> port_;
  Config config_;
  Snapshot acc_{};
  std::size_t next_sensor_{0U};
  ::bno08x::OpId pending_op_{0U};
  ::bno08x::Error config_error_{::bno08x::Error::Ok};
  bool pending_{false};
  bool restart_seen_{false};
};

/// @name App API (OwnerAccess)
/// @{

/** @brief Runs `c` and returns its operation number. */
template <typename Access>
[[nodiscard]] kb::Result<::bno08x::OpId> Submit(Access& io, const Command& c,
                                                kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::bno08x::OpId>::Success(r.Value().op) : Forward<::bno08x::OpId>(r);
}

/** @brief Set Feature of `sensor` (an interval of 0 turns it off). */
template <typename Access>
[[nodiscard]] kb::Result<::bno08x::OpId> SetSensor(Access& io, ::bno08x::SensorId sensor,
                                                   const ::bno08x::FeatureConfig& feature,
                                                   kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetSensor;
  c.sensor = sensor;
  c.feature = feature;
  return Submit(io, c, timeout);
}

/** @brief Tare `axes` on `basis` now. */
template <typename Access>
[[nodiscard]] kb::Result<::bno08x::OpId> Tare(Access& io, ::bno08x::AxisMask axes,
                                              ::bno08x::TareBasis basis,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Tare;
  c.axes = axes;
  c.basis = basis;
  return Submit(io, c, timeout);
}

/** @brief Executable reset: the hub restarts and the driver restores its sensors. */
template <typename Access>
[[nodiscard]] kb::Result<::bno08x::OpId> ResetHub(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ResetHub;
  return Submit(io, c, timeout);
}

/** @brief Request the product ID (it arrives in `Snapshot::product`). */
template <typename Access>
[[nodiscard]] kb::Result<::bno08x::OpId> ReadProductId(Access& io,
                                                       kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadProductId;
  return Submit(io, c, timeout);
}

/** @brief The record of `op` in `snap`, if it is there. */
[[nodiscard]] inline std::optional<OpRecord> FindOp(const Snapshot& snap,
                                                    ::bno08x::OpId op) noexcept {
  std::optional<OpRecord> found{};
  for (std::size_t i = 0U; i < kOpHistory; ++i) {
    const OpRecord& r = snap.ops[i];
    if (r.valid && (r.op == op)) {
      found = r;
    }
  }
  return found;
}

/**
 * @brief Waits (polling `Latest()`, 1 ms apart) until the record of `op` is published.
 * @return The record when the operation succeeded; its error (status and driver code) when it
 * failed; Timeout when no record came within `timeout`. Never call it on the owner task.
 */
template <typename Access>
[[nodiscard]] kb::Result<OpRecord> WaitOp(Access& io, ::bno08x::OpId op,
                                          kb::Milliseconds timeout) noexcept {
  const kb::utils::Deadline deadline = kb::utils::Deadline::After(kb::rtos::Now(), timeout);
  std::optional<OpRecord> found{};
  bool waiting = true;
  while (waiting) {
    const auto snap = io.Latest();
    if (snap.IsOk()) {
      found = FindOp(snap.Value().value, op);
    }
    waiting = (!found.has_value()) && (!deadline.Expired(kb::rtos::Now()));
    if (waiting) {
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
  }
  kb::Result<OpRecord> result = kb::Result<OpRecord>::Failure(kb::Status::Timeout);
  if (found.has_value()) {
    const ::bno08x::Error e = found->error;
    result = (e == ::bno08x::Error::Ok) ? kb::Result<OpRecord>::Success(*found)
                                        : kb::Result<OpRecord>::Failure(ToStatus(e), DetailOf(e));
  }
  return result;
}

/** @brief The latest sample of `sensor` from the newest snapshot; NotReady before one arrived. */
template <typename Access>
[[nodiscard]] kb::Result<Sample> LatestSample(const Access& io,
                                              ::bno08x::SensorId sensor) noexcept {
  kb::Result<Sample> result = kb::Result<Sample>::Failure(kb::Status::NotReady);
  const auto snap = io.Latest();
  if (snap.IsOk()) {
    for (std::size_t i = 0U; i < kMaxSensors; ++i) {
      const Sample& s = snap.Value().value.samples[i];
      if (s.valid && (s.sensor == sensor)) {
        result = kb::Result<Sample>::Success(s);
      }
    }
  } else {
    result = Forward<Sample>(snap);
  }
  return result;
}

/// @}

} // namespace kb::handlers::bno08x

#endif // KB_HANDLERS_BINDINGS_BNO08X_HPP
