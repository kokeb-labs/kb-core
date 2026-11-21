// clang-format off: the table below must keep one row per line.
/**
 * @file bmp581.hpp
 * @brief The bmp581 binding and handler: BMP581 barometric pressure sensor on I2C or SPI.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `I2cPort<Bus>` (I2cHooks, with the optional `GetLastFailure` so an absent chip is `NoDevice`) or `SpiPort<Dev>` (SpiHooks, four-wire); both with `DelayUs` through the hook timing (a spin below 1 ms, a sleep from 1 ms). `Handler<Bus>` picks the port from the bus type |
 * | Errors | `ToStatus()`: every `bmp581::Error` to a kb::Status; the code in the health record's detail |
 * | Start-up | `Identify + Configure` = `driver.EnsureInitialized()` (sync): power-up wait, soft reset, CHIP_ID, NVM ready, configuration written and read back, configured mode entered |
 * | Thread model | Guarded (`kRequiresOwner = false`): a slow sensor read now and then from several tasks; an owner bus samples it at `sample_period` |
 * | Health | `DeviceReset`, `NotInitialized` and `StateUnknown` (the configuration is in doubt until a new start) fault at once; other failures count to the threshold |
 * | App API | snapshot `{pressure, temperature}` (forced measurement in standby, data registers otherwise); commands: measure, read, power mode, reset check, interrupt status, FIFO count, raw register |
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c0{bus};
 * kb::handlers::Guarded<kb::handlers::bmp581::Handler<Bus>, decltype(i2c0)> baro{i2c0, bus, timing, cfg};
 * if (baro.EnsureStarted(kb::Milliseconds{50U}).IsOk()) {
 *   const auto s = baro.Sample(kb::Milliseconds{50U});  // s.Value().value.pressure_pa
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_BINDINGS_BMP581_HPP
#define KB_HANDLERS_BINDINGS_BMP581_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <bmp581.hpp>

namespace kb::handlers::bmp581 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::bmp581::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::bmp581::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::bmp581::Error::NotInitialized:
  case ::bmp581::Error::DeviceReset:
  case ::bmp581::Error::NoData:
    s = kb::Status::NotReady;
    break;
  case ::bmp581::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::bmp581::Error::Bus:
  case ::bmp581::Error::InvalidReply:
    s = kb::Status::BusError;
    break;
  case ::bmp581::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::bmp581::Error::DeviceIdMismatch:
  case ::bmp581::Error::DeviceFault:
  case ::bmp581::Error::WriteMismatch:
  case ::bmp581::Error::StateUnknown:
  case ::bmp581::Error::ConfigRejected:
    s = kb::Status::Fault;
    break;
  case ::bmp581::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::bmp581::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::bmp581::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::bmp581::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  case ::bmp581::Error::OutOfRange:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief True for a driver error that only a new start clears (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::bmp581::Error error) noexcept {
  return (error == ::bmp581::Error::DeviceReset) || (error == ::bmp581::Error::NotInitialized) ||
         (error == ::bmp581::Error::StateUnknown);
}

/**
 * @brief The driver's I2C port over a kb::iface::I2cBus.
 * @tparam Bus The bus (an I2cBus or I2cBusRef), 100 kHz to 1 MHz.
 */
template <kb::iface::I2cBus Bus>
class I2cPort : public ::bmp581::I2cInterface<I2cPort<Bus>>, public I2cHooks<I2cPort<Bus>, Bus> {
public:
  /** @brief Over `bus` with `timing`; no I/O. */
  I2cPort(Bus& bus, const HookTiming& timing) noexcept : I2cHooks<I2cPort<Bus>, Bus>(bus, timing) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transfer. */
  bool Write(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: write then read. */
  bool WriteRead(uint8_t addr, const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                 std::size_t rx_len) noexcept {
    return this->BusWriteRead(addr, tx, tx_len, rx, rx_len);
  }
  /** @brief Optional hook: why the last transfer failed. */
  bool GetLastFailure(::bmp581::I2cFailure& failure) noexcept {
    const kb::iface::I2cFailure f = this->BusLastFailure();
    switch (f) {
    case kb::iface::I2cFailure::AddressNack:
      failure = ::bmp581::I2cFailure::AddressNack;
      break;
    case kb::iface::I2cFailure::DataNack:
      failure = ::bmp581::I2cFailure::DataNack;
      break;
    case kb::iface::I2cFailure::None:
    case kb::iface::I2cFailure::Bus:
    case kb::iface::I2cFailure::Timeout:
    case kb::iface::I2cFailure::BusStuck:
      failure = ::bmp581::I2cFailure::Other;
      break;
    }
    return f != kb::iface::I2cFailure::None;
  }
  /**
   * @brief Hook: wait at least `us` microseconds. A sleep (rounded up to the scheduler tick), not
   * the hook timing's spin: HookTiming keeps no valid spin reference (see the doc page).
   */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
};

/**
 * @brief The driver's four-wire SPI port over a kb::iface::SpiDevice.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class SpiPort : public ::bmp581::SpiInterface<SpiPort<Dev>>, public SpiHooks<SpiPort<Dev>, Dev> {
public:
  /** @brief Over `dev` with `timing`; no I/O. */
  SpiPort(Dev& dev, const HookTiming& timing) noexcept : SpiHooks<SpiPort<Dev>, Dev>(dev, timing) {}

  /** @brief Hook: bring the device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one full-duplex frame (`rx` may be null). */
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
};

/** @brief Which port and bus kind a bus type selects; I2C first. */
template <typename Bus>
struct Transport;

/** @brief An I2C bus: I2cPort. */
template <kb::iface::I2cBus Bus>
struct Transport<Bus> {
  using PortType = I2cPort<Bus>;                 ///< The port.
  static constexpr BusKind kKind = BusKind::I2c; ///< The bus kind.
};

/** @brief An SPI device: SpiPort. */
template <kb::iface::SpiDevice Bus>
  requires(!kb::iface::I2cBus<Bus>)
struct Transport<Bus> {
  using PortType = SpiPort<Bus>;                 ///< The port.
  static constexpr BusKind kKind = BusKind::Spi; ///< The bus kind.
};

/** @brief The port a handler over `Bus` uses. */
template <typename Bus>
using Port = typename Transport<Bus>::PortType;

/** @brief The driver type a handler over `Bus` owns. */
template <typename Bus>
using Driver = ::bmp581::BMP581<Port<Bus>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::bmp581::Config chip{};               ///< What EnsureInitialized() writes.
  HandlerConfig handler{};               ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{100U}; ///< Bound of the start-up stage (about 7 ms typical).
  ::bmp581::Verify sample_verify{::bmp581::Verify::None}; ///< Measure(): read the data twice.
  uint8_t address{::bmp581::i2c::kI2cAddress}; ///< 7-bit I2C address (0x46 or 0x47); unused on SPI.
};

/** @brief What a sample publishes. */
struct Snapshot {
  int32_t pressure_raw{0};    ///< Pressure in 1/64 Pa.
  int32_t temperature_raw{0}; ///< Temperature in 1/65536 degC.
  float pressure_pa{0.0F};    ///< Pressure in pascals.
  float temperature_c{0.0F};  ///< Temperature in degrees Celsius.
  bool forced{false};         ///< True when the sample was a forced measurement (standby mode).
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  MeasureOnce = 0,            ///< One forced measurement (standby mode only).
  ReadMeasurement = 1,        ///< The data registers now.
  SetPowerMode = 2,           ///< Power mode to `mode`.
  CheckForReset = 3,          ///< Find out whether the chip reset since the start.
  ReadInterruptStatus = 4,    ///< INT_STATUS, raw (`Reply::raw`); the read clears it.
  ReadFifoCount = 5,          ///< Frames waiting in the FIFO (`Reply::raw`).
  ReadRegister = 6,           ///< Register `address` once (`Reply::raw`).
  ClearLatchedInterrupts = 7, ///< Empty the driver's INT_STATUS latch (no I/O).
};

/** @brief One command. */
struct Command {
  Op op{Op::ReadMeasurement};                             ///< What to do.
  ::bmp581::PowerMode mode{::bmp581::PowerMode::Standby}; ///< For SetPowerMode.
  ::bmp581::Verify verify{::bmp581::Verify::None};        ///< For the measurements.
  uint8_t address{0U};                                    ///< For ReadRegister.
};

/** @brief The reply of a command. */
struct Reply {
  int32_t pressure_raw{0};    ///< The measurements: pressure in 1/64 Pa.
  int32_t temperature_raw{0}; ///< The measurements: temperature in 1/65536 degC.
  uint8_t raw{0U};            ///< The byte reads; 0 otherwise.
};

/**
 * @brief The bmp581 handler.
 * @tparam Bus The bus: an I2cBus (or I2cBusRef) or an SpiDevice (or SpiDeviceRef).
 */
template <typename Bus>
class Handler : public HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false;          ///< Served by occasional calls.
  static constexpr BusKind kBus = Transport<Bus>::kKind; ///< I2C or SPI, from the bus type.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Bus>>& slot) noexcept {
    if constexpr (kBus == BusKind::I2c) {
      slot.emplace(port_, config_.address, config_.chip);
    } else {
      slot.emplace(port_, config_.chip);
    }
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Bus>& driver) noexcept {
    static_cast<void>(call);
    const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Bus>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::bmp581::Error::DeviceReset)) ||
           (detail == DetailOf(::bmp581::Error::NotInitialized)) ||
           (detail == DetailOf(::bmp581::Error::StateUnknown));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Bus>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const bool forced = driver.GetConfig().mode == ::bmp581::PowerMode::Standby;
    const kb::Result<::bmp581::Measurement> m =
        forced ? Lift(driver.MeasureOnce(config_.sample_verify), &ToStatus)
               : Lift(driver.ReadMeasurement(config_.sample_verify), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(m);
    if (m.IsOk()) {
      Snapshot s{};
      s.pressure_raw = m.Value().pressure_raw;
      s.temperature_raw = m.Value().temperature_raw;
      s.pressure_pa = ::bmp581::PressurePa(s.pressure_raw);
      s.temperature_c = ::bmp581::TemperatureC(s.temperature_raw);
      s.forced = forced;
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Bus>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::MeasureOnce:
      result = measurement(Lift(driver.MeasureOnce(c.verify), &ToStatus));
      break;
    case Op::ReadMeasurement:
      result = measurement(Lift(driver.ReadMeasurement(c.verify), &ToStatus));
      break;
    case Op::SetPowerMode:
      result = done(LiftStatus(driver.SetPowerMode(c.mode), &ToStatus));
      break;
    case Op::CheckForReset:
      result = done(LiftStatus(driver.CheckForReset(), &ToStatus));
      break;
    case Op::ReadInterruptStatus:
      result = interrupts(Lift(driver.ReadInterruptStatus(), &ToStatus), driver);
      break;
    case Op::ReadFifoCount:
      result = byte(Lift(driver.ReadFifoCount(), &ToStatus));
      break;
    case Op::ReadRegister:
      result = byte(Lift(driver.ReadRegister(c.address), &ToStatus));
      break;
    case Op::ClearLatchedInterrupts:
      driver.ClearLatchedInterrupts();
      result = kb::Result<Reply>::Success(Reply{});
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.Counters();
  }

private:
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> byte(const kb::Result<uint8_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.raw = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> measurement(
      const kb::Result<::bmp581::Measurement>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.pressure_raw = r.Value().pressure_raw;
      reply.temperature_raw = r.Value().temperature_raw;
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> interrupts(const kb::Result<::bmp581::InterruptStatus>& r,
                                                    const Driver<Bus>& driver) noexcept {
    return r.IsOk() ? byte(kb::Result<uint8_t>::Success(driver.GetLastIntStatus()))
                    : Forward<Reply>(r);
  }

  Port<Bus> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief One forced measurement now, in the chip's units (standby mode only). */
template <typename Access>
[[nodiscard]] kb::Result<::bmp581::Measurement> MeasureOnce(Access& io,
                                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::MeasureOnce;
  const kb::Result<Reply> r = io.Request(c, timeout);
  kb::Result<::bmp581::Measurement> result = Forward<::bmp581::Measurement>(r);
  if (r.IsOk()) {
    ::bmp581::Measurement m{};
    m.pressure_raw = r.Value().pressure_raw;
    m.temperature_raw = r.Value().temperature_raw;
    result = kb::Result<::bmp581::Measurement>::Success(m);
  }
  return result;
}

/** @brief The pressure in pascals of a forced measurement now (standby mode only). */
template <typename Access>
[[nodiscard]] kb::Result<float> ReadPressurePa(Access& io, kb::Milliseconds timeout) noexcept {
  const kb::Result<::bmp581::Measurement> m = MeasureOnce(io, timeout);
  return m.IsOk() ? kb::Result<float>::Success(::bmp581::PressurePa(m.Value().pressure_raw))
                  : Forward<float>(m);
}

/** @brief The power mode: Standby, Normal or Continuous. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetPowerMode(Access& io, ::bmp581::PowerMode mode,
                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetPowerMode;
  c.mode = mode;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Find out whether the chip reset since the start (a reset faults the handler). */
template <typename Access>
[[nodiscard]] kb::Result<void> CheckForReset(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::CheckForReset;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::bmp581

#endif // KB_HANDLERS_BINDINGS_BMP581_HPP
