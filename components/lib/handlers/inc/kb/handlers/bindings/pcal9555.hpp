/**
 * @file pcal9555.hpp
 * @brief The pcal9555 binding and handler: PCA9555 / PCAL9555A 16-bit I2C GPIO expander.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Bus>`: the driver's I2C hooks over a kb::iface::I2cBus (I2cHooks), with the
 * optional `GetLastFailure` so an absent chip is `NoDevice` | | Errors | `ToStatus()`: every
 * `pcal9555::Error` to a kb::Status; the code in the health record's detail | | Start-up |
 * `Identify + Configure` = `driver.EnsureInitialized()` (sync): presence, identity pattern, every
 * register written and read back | | Thread model | Guarded (`kRequiresOwner = false`): a slow
 * expander used from several tasks; an owner bus works too | | Health | `DeviceReset` (the chip
 * lost its registers) and `NotInitialized` fault at once; other failures count to the threshold |
 * | App API | snapshot `{inputs, outputs, interrupts}`; commands: write, set, toggle outputs,
 * pulls, latches, output mode, reset check |
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded> i2c0{bus};
 * kb::handlers::Guarded<kb::handlers::pcal9555::Handler<Bus>, decltype(i2c0)> io{i2c0, bus, timing,
 * cfg}; if (io.EnsureStarted(kb::Milliseconds{20U}).IsOk()) {
 *   static_cast<void>(kb::handlers::pcal9555::WriteOutputs(io, 0x00FFU, kb::Milliseconds{5U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_PCAL9555_HPP
#define KB_HANDLERS_BINDINGS_PCAL9555_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <pcal9555.hpp>

namespace kb::handlers::pcal9555 {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::pcal9555::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::pcal9555::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::pcal9555::Error::NotInitialized:
  case ::pcal9555::Error::DeviceReset:
    s = kb::Status::NotReady;
    break;
  case ::pcal9555::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::pcal9555::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::pcal9555::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::pcal9555::Error::DeviceIdMismatch:
  case ::pcal9555::Error::WriteMismatch:
  case ::pcal9555::Error::DeviceFault:
    s = kb::Status::Fault;
    break;
  case ::pcal9555::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::pcal9555::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::pcal9555::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::pcal9555::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's state is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::pcal9555::Error error) noexcept {
  return (error == ::pcal9555::Error::DeviceReset) || (error == ::pcal9555::Error::NotInitialized);
}

/**
 * @brief The driver's I2C port over a kb::iface::I2cBus.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Port : public ::pcal9555::I2cInterface<Port<Bus>>, public I2cHooks<Port<Bus>, Bus> {
public:
  /** @brief Over `bus` with `timing`; no I/O. */
  Port(Bus& bus, const HookTiming& timing) noexcept : I2cHooks<Port<Bus>, Bus>(bus, timing) {}

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transfer. */
  bool Write(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: write then read with a repeated START. */
  bool WriteRead(uint8_t addr, const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                 std::size_t rx_len) noexcept {
    return this->BusWriteRead(addr, tx, tx_len, rx, rx_len);
  }
  /** @brief Optional hook: why the last transfer failed. */
  bool GetLastFailure(::pcal9555::I2cFailure& failure) noexcept {
    const kb::iface::I2cFailure f = this->BusLastFailure();
    switch (f) {
    case kb::iface::I2cFailure::AddressNack:
      failure = ::pcal9555::I2cFailure::AddressNack;
      break;
    case kb::iface::I2cFailure::DataNack:
      failure = ::pcal9555::I2cFailure::DataNack;
      break;
    case kb::iface::I2cFailure::None:
    case kb::iface::I2cFailure::Bus:
    case kb::iface::I2cFailure::Timeout:
    case kb::iface::I2cFailure::BusStuck:
      failure = ::pcal9555::I2cFailure::Other;
      break;
    }
    return f != kb::iface::I2cFailure::None;
  }
};

/** @brief The driver type a handler over `Bus` owns. */
template <kb::iface::I2cBus Bus>
using Driver = ::pcal9555::PCAL9555<Port<Bus>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::pcal9555::Config chip{};                ///< What EnsureInitialized() writes.
  HandlerConfig handler{};                  ///< Health, recovery, polling.
  kb::Milliseconds start_deadline{50U};     ///< Bound of the start-up stage.
  uint8_t address{::pcal9555::kI2cAddress}; ///< 7-bit address (0x20 to 0x27).
};

/** @brief What a sample publishes. */
struct Snapshot {
  uint16_t inputs{0U};     ///< Input port levels (after polarity inversion), pin n at bit n.
  uint16_t outputs{0U};    ///< The output levels the driver last set (its configuration).
  uint16_t interrupts{0U}; ///< Pins whose interrupt status was seen since the last clear.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  WriteOutputs = 0,   ///< Every output: `mask` bits high, the others low.
  SetOutputs = 1,     ///< Outputs in `mask` to `high`.
  ToggleOutputs = 2,  ///< Outputs in `mask` inverted.
  ReadInputs = 3,     ///< Read the input ports now.
  SetPull = 4,        ///< Pulls of `mask` to `pull` (PCAL9555A).
  SetInputLatch = 5,  ///< Input latch of `mask` to `high` (PCAL9555A).
  SetOutputMode = 6,  ///< Output stage of `bank` to `mode` (PCAL9555A).
  ReadInterrupts = 7, ///< Read and clear the interrupt status (PCAL9555A).
  CheckForReset = 8,  ///< Check the chip still holds its configuration.
};

/** @brief One command. */
struct Command {
  uint16_t mask{0U};                                             ///< Pins, bit n = pin n.
  Op op{Op::ReadInputs};                                         ///< What to do.
  bool high{false};                                              ///< Level or latch on/off.
  ::pcal9555::Verify verify{::pcal9555::Verify::None};           ///< Read back after writing.
  ::pcal9555::Pull pull{::pcal9555::Pull::Up};                   ///< For SetPull.
  ::pcal9555::Bank bank{::pcal9555::Bank::Port0};                ///< For SetOutputMode.
  ::pcal9555::OutputMode mode{::pcal9555::OutputMode::PushPull}; ///< For SetOutputMode.
};

/** @brief The reply of a command. */
struct Reply {
  uint16_t value{0U}; ///< Inputs or interrupt status for the reads; 0 otherwise.
};

/**
 * @brief The pcal9555 handler.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Handler : public HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::I2c; ///< On an I2C bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Bus>>& slot) noexcept {
    slot.emplace(port_, config_.address, config_.chip);
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
    return (detail == DetailOf(::pcal9555::Error::DeviceReset)) ||
           (detail == DetailOf(::pcal9555::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Bus>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    const kb::Result<uint16_t> inputs = Lift(driver.ReadInputs(), &ToStatus);
    kb::Result<Snapshot> result = Forward<Snapshot>(inputs);
    if (inputs.IsOk()) {
      Snapshot s{};
      s.inputs = inputs.Value();
      s.outputs = driver.GetConfig().output_high_mask;
      s.interrupts = driver.GetLatchedInterrupts();
      result = kb::Result<Snapshot>::Success(s);
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Bus>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::WriteOutputs:
      result = done(LiftStatus(driver.WriteOutputs(c.mask, c.verify), &ToStatus));
      break;
    case Op::SetOutputs:
      result = done(LiftStatus(driver.SetOutputs(c.mask, c.high, c.verify), &ToStatus));
      break;
    case Op::ToggleOutputs:
      result = done(LiftStatus(driver.ToggleOutputs(c.mask, c.verify), &ToStatus));
      break;
    case Op::ReadInputs:
      result = value(Lift(driver.ReadInputs(), &ToStatus));
      break;
    case Op::SetPull:
      result = done(LiftStatus(driver.SetPull(c.mask, c.pull, c.verify), &ToStatus));
      break;
    case Op::SetInputLatch:
      result = done(LiftStatus(driver.SetInputLatch(c.mask, c.high, c.verify), &ToStatus));
      break;
    case Op::SetOutputMode:
      result = done(LiftStatus(driver.SetOutputMode(c.bank, c.mode, c.verify), &ToStatus));
      break;
    case Op::ReadInterrupts:
      result = value(Lift(driver.ReadInterruptStatus(), &ToStatus));
      break;
    case Op::CheckForReset:
      result = done(LiftStatus(driver.CheckForReset(), &ToStatus));
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
  [[nodiscard]] static kb::Result<Reply> value(const kb::Result<uint16_t>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.value = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Bus> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Every output: `high_mask` bits high, the others low. */
template <typename Access>
[[nodiscard]] kb::Result<void> WriteOutputs(
    Access& io, uint16_t high_mask, kb::Milliseconds timeout,
    ::pcal9555::Verify verify = ::pcal9555::Verify::None) noexcept {
  Command c{};
  c.op = Op::WriteOutputs;
  c.mask = high_mask;
  c.verify = verify;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Outputs in `mask` to `high`. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetOutputs(Access& io, uint16_t mask, bool high,
                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetOutputs;
  c.mask = mask;
  c.high = high;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The input ports, read now. */
template <typename Access>
[[nodiscard]] kb::Result<uint16_t> ReadInputs(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadInputs;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint16_t>::Success(r.Value().value) : Forward<uint16_t>(r);
}

/// @}

} // namespace kb::handlers::pcal9555

#endif // KB_HANDLERS_BINDINGS_PCAL9555_HPP
