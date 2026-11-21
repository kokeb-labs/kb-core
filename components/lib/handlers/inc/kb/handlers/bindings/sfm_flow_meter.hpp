/**
 * @file sfm_flow_meter.hpp
 * @brief The sfm_flow_meter binding and handler: SFM3000 to SFM6000D I2C mass flow meters.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Bus>`: the driver's I2C hooks over a kb::iface::I2cBus (I2cHooks), the optional
 * general call, `DelayUs` and `NowUs` from the hook timing, and the /RES pin (GpioHooks, absent
 * unless the HAL supplies it) | | Errors | `ToStatus()`: every `sfm::Error` to a kb::Status; the
 * code in the health record's detail | | Start-up | `Boot + Identify` = `BeginInitialize()` then
 * `Poll()` until done (polled: wake, product identifier, averaging); `Configure` =
 * `StartMeasurement(gas)` then `Poll()` until the first sample (polled, optional) | | Thread model
 * | Owner by preference (`kRequiresOwner = false`): a flow meter is read at its sample rate by one
 * loop and its waits are polled, so the owner never blocks on a start; a guarded bus works too | |
 * Health | `DeviceReset` (the measurement ended by itself), `StateUnknown` and `NotInitialized`
 * fault at once; `NoData` is not a failure (the snapshot is marked not fresh) | | App API |
 * snapshot `{sample, state, fresh}`; commands: start and stop the measurement, averaging, oxygen
 * fraction, temperature read, sleep, wake, soft and hard reset, one sample now |
 *
 * @code
 * kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Owner> i2c1{bus};
 * kb::handlers::OwnerAccess<kb::handlers::sfm_flow_meter::Handler<Bus>, decltype(i2c1)> flow{i2c1,
 * bus, timing, cfg}; flow.RequestStart();                                 // the owner task runs
 * the plan const auto s = flow.Latest();                        // lock-free, newest sample if
 * (s.IsOk() && s.Value().value.fresh) { use(s.Value().value.sample.flow_ml_per_min); }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_SFM_FLOW_METER_HPP
#define KB_HANDLERS_BINDINGS_SFM_FLOW_METER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <sfm_flow_meter.hpp>

namespace kb::handlers::sfm_flow_meter {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::sfm::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::sfm::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::sfm::Error::NotInitialized:
  case ::sfm::Error::DeviceReset:
  case ::sfm::Error::StateUnknown:
  case ::sfm::Error::Stale:
    s = kb::Status::NotReady;
    break;
  case ::sfm::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::sfm::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::sfm::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::sfm::Error::DeviceIdMismatch:
  case ::sfm::Error::DeviceFault:
  case ::sfm::Error::StatusMismatch:
  case ::sfm::Error::InvalidReply:
    s = kb::Status::Fault;
    break;
  case ::sfm::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::sfm::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::sfm::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::sfm::Error::NoData:
    s = kb::Status::Empty;
    break;
  case ::sfm::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  case ::sfm::Error::OutOfRange:
    s = kb::Status::OutOfRange;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the chip's mode is lost (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::sfm::Error error) noexcept {
  return (error == ::sfm::Error::DeviceReset) || (error == ::sfm::Error::StateUnknown) ||
         (error == ::sfm::Error::NotInitialized);
}

/** @brief The control output index of the /RES pin (SFM4200) in the port's GpioHooks. */
inline constexpr std::size_t kResetPin = 0U;

/**
 * @brief The driver's I2C port over a kb::iface::I2cBus.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Port : public ::sfm::I2cInterface<Port<Bus>>,
             public I2cHooks<Port<Bus>, Bus>,
             public GpioHooks<Port<Bus>, 1U, 0U> {
public:
  /** @brief Over `bus` with `timing` and the optional /RES pin; no I/O. */
  Port(Bus& bus, const HookTiming& timing, const OutputPin& reset) noexcept
      : I2cHooks<Port<Bus>, Bus>(bus, timing),
        GpioHooks<Port<Bus>, 1U, 0U>(std::array<OutputPin, 1U>{reset}, std::array<InputPin, 0U>{}) {
  }

  /** @brief Hook: bring the bus up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one write transfer. */
  bool Write(uint8_t addr, const uint8_t* data, std::size_t len) noexcept {
    return this->BusWrite(addr, data, len);
  }
  /** @brief Hook: one read transfer (an address NACK is the chip's "no data yet"). */
  bool Read(uint8_t addr, uint8_t* data, std::size_t len) noexcept {
    return this->BusRead(addr, data, len);
  }
  /** @brief Optional hook: the general call (address 0), used only with `allow_general_call_reset`.
   */
  bool WriteGeneralCall(uint8_t byte) noexcept {
    const std::array<uint8_t, 1U> frame{byte};
    return this->BusWrite(0U, frame.data(), frame.size());
  }
  /** @brief Optional hook: drive /RES; false when the HAL supplied no pin. */
  bool SetResetPin(bool asserted) noexcept {
    return this->DriveOutput(kResetPin, asserted);
  }
  /** @brief Hook: wait at least `us`. */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: the free-running microsecond clock. */
  uint32_t NowUs() noexcept {
    return this->Timing().NowUs32();
  }
  /** @brief The I2C transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return I2cHooks<Port<Bus>, Bus>::Counters();
  }
};

/** @brief The driver type a handler over `Bus` owns. */
template <kb::iface::I2cBus Bus>
using Driver = ::sfm::SfmFlowMeter<Port<Bus>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::sfm::Config chip{};                     ///< Part, family, address, averaging, policies.
  HandlerConfig handler{};                  ///< Health, recovery, polling, sample period.
  kb::Milliseconds identify_deadline{200U}; ///< Bound of the wake and identification stage.
  kb::Milliseconds start_deadline{200U};    ///< Bound of the measurement start stage.
  bool measure_on_start{true};              ///< Run the `Configure` stage (start `gas`).
  ::sfm::Gas gas{::sfm::Gas::Air};          ///< The gas the start-up measures.
  uint16_t o2_permille{0U};                 ///< Mixtures: the oxygen fraction of `gas`.
  OutputPin reset{};                        ///< /RES (SFM4200); absent on most boards.
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::sfm::Sample sample{}; ///< The newest sample read (kept when not fresh).
  ::sfm::State state{::sfm::State::Uninitialized}; ///< The driver's mode at this sample.
  bool fresh{
      false}; ///< True when `sample` was read by this sample; false: none new or not measuring.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  StartMeasurement = 0,    ///< Start `gas` (with `value` = oxygen per mille for mixtures).
  StopMeasurement = 1,     ///< Stop the measurement.
  SetAveraging = 2,        ///< Averaging to `value` while idle.
  UpdateConcentration = 3, ///< Oxygen fraction of a running mixture to `value`.
  ReadTemperature = 4,     ///< Read the temperature by a pointer sequence (parts that offer it).
  Sleep = 5,               ///< Enter sleep mode (idle only).
  WakeUp = 6,              ///< Wake from sleep.
  SoftReset = 7,           ///< Soft reset, then identify again and restart the configured gas.
  HardReset = 8,           ///< /RES or supply cycle, then identify again and restart.
  ReadSample = 9,          ///< One sample now.
};

/** @brief One command. */
struct Command {
  Op op{Op::ReadSample};           ///< What to do.
  ::sfm::Gas gas{::sfm::Gas::Air}; ///< For StartMeasurement.
  uint16_t value{0U};              ///< Oxygen per mille, or the averaging N.
};

/** @brief The reply of a command. */
struct Reply {
  ::sfm::Sample sample{};                          ///< ReadSample: the sample.
  int32_t temperature_mdegc{0};                    ///< ReadTemperature: milli-degrees Celsius.
  ::sfm::State state{::sfm::State::Uninitialized}; ///< The driver's mode after the command.
};

/**
 * @brief The sfm_flow_meter handler.
 * @tparam Bus The bus (an I2cBus or I2cBusRef).
 */
template <kb::iface::I2cBus Bus>
class Handler : public HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Bus>, Driver<Bus>, Snapshot, Command, Reply>;
  using Wait = kb::Result<::sfm::Wait>;

public:
  /// The start-up plan: identification polled to its end, then the measurement start (optional).
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{static_cast<uint16_t>(stage::kBoot | stage::kIdentify), StageMode::Polled, false},
       {stage::kConfigure, StageMode::Polled, true}}};
  static constexpr bool kRequiresOwner = false; ///< Served by an owner loop or occasional calls.
  static constexpr BusKind kBus = BusKind::I2c; ///< On an I2C bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Bus& bus, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(bus, timing, config.reset), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Bus>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Bus>& driver) noexcept {
    Wait step = Wait::Failure(kb::Status::Fault);
    if (call.calls != 0U) {
      step = Lift(driver.Poll(), &ToStatus);
    } else if (call.index == 0U) {
      step = Lift(driver.BeginInitialize(), &ToStatus);
    } else {
      step = Lift(driver.StartMeasurement(config_.gas, config_.o2_permille), &ToStatus);
    }
    kb::Result<StartProgress> result = Forward<StartProgress>(step);
    if (step.IsOk()) {
      const StartProgress p = driver.HasPending() ? StartProgress::Pending : StartProgress::Done;
      result = kb::Result<StartProgress>::Success(p);
    }
    return result;
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.identify_deadline : config_.start_deadline;
  }
  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept {
    return (index != 1U) || config_.measure_on_start;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Bus>& driver) noexcept {
    kb::Result<void> stopped = kb::Result<void>::Success();
    if ((driver.GetState() == ::sfm::State::Measuring) && (!driver.HasPending())) {
      stopped = settle(driver, driver.Stop());
    }
    const kb::Result<void> forgot = LiftStatus(driver.Deinitialize(), &ToStatus);
    return stopped.IsOk() ? forgot : stopped;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return (detail == DetailOf(::sfm::Error::DeviceReset)) ||
           (detail == DetailOf(::sfm::Error::StateUnknown)) ||
           (detail == DetailOf(::sfm::Error::NotInitialized));
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Bus>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    Snapshot s = last_;
    s.fresh = false;
    s.state = driver.GetState();
    kb::Result<Snapshot> result = kb::Result<Snapshot>::Success(s);
    if ((s.state == ::sfm::State::Measuring) && (!driver.HasPending())) {
      const ::sfm::Result<::sfm::Sample> r = driver.ReadSample();
      if (r.ok()) {
        s.sample = r.value;
        s.fresh = true;
        last_ = s;
        result = kb::Result<Snapshot>::Success(s);
      } else if (r.error == ::sfm::Error::NoData) {
        // No new sample yet: the sensor at work, not a failure.
      } else {
        result = kb::Result<Snapshot>::Failure(ToStatus(r.error), DetailOf(r.error));
      }
    }
    return result;
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Bus>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<void> r = kb::Result<void>::Failure(kb::Status::InvalidArgument);
    Reply reply{};
    switch (c.op) {
    case Op::StartMeasurement:
      r = settle(driver, driver.StartMeasurement(c.gas, c.value));
      break;
    case Op::StopMeasurement:
      r = settle(driver, driver.Stop());
      break;
    case Op::SetAveraging:
      r = settle(driver, driver.SetAveraging(c.value));
      break;
    case Op::UpdateConcentration:
      r = settle(driver, driver.UpdateConcentration(c.value));
      break;
    case Op::ReadTemperature:
      r = settle(driver, driver.ReadTemperature());
      reply.temperature_mdegc = driver.GetTemperature();
      break;
    case Op::Sleep:
      r = settle(driver, driver.Sleep());
      break;
    case Op::WakeUp:
      r = settle(driver, driver.WakeUp());
      break;
    case Op::SoftReset:
      r = restart(driver, settle(driver, driver.SoftReset()));
      break;
    case Op::HardReset:
      r = restart(driver, settle(driver, driver.HardReset()));
      break;
    case Op::ReadSample:
      r = sampleNow(driver, reply);
      break;
    }
    reply.state = driver.GetState();
    return r.IsOk() ? kb::Result<Reply>::Success(reply) : Forward<Reply>(r);
  }
  /// @}

  /** @brief The port's I2C transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  // An operation that may wait, finished with the driver's own bounded wait (port DelayUs).
  [[nodiscard]] static kb::Result<void> settle(Driver<Bus>& driver,
                                               const ::sfm::Result<::sfm::Wait>& begun) noexcept {
    kb::Result<void> r = kb::Result<void>::Failure(ToStatus(begun.error), DetailOf(begun.error));
    if (begun.ok()) {
      r = LiftStatus(driver.AwaitPending(), &ToStatus);
    }
    return r;
  }
  // After a reset the driver is uninitialised: identify again and restart the configured gas.
  [[nodiscard]] kb::Result<void> restart(Driver<Bus>& driver,
                                         const kb::Result<void>& reset) noexcept {
    kb::Result<void> r = reset;
    if (r.IsOk()) {
      r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    }
    if (r.IsOk() && config_.measure_on_start) {
      r = settle(driver, driver.StartMeasurement(config_.gas, config_.o2_permille));
    }
    return r;
  }
  [[nodiscard]] static kb::Result<void> sampleNow(Driver<Bus>& driver, Reply& reply) noexcept {
    const ::sfm::Result<::sfm::Sample> s = driver.ReadSample();
    reply.sample = s.value;
    return LiftStatus(s, &ToStatus);
  }

  Port<Bus> port_;
  Config config_;
  Snapshot last_{};
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Runs `c` and drops the reply's values. */
template <typename Access>
[[nodiscard]] kb::Result<void> Send(Access& io, const Command& c,
                                    kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Starts measuring `gas` (`o2_permille` for mixtures) and waits for the first sample. */
template <typename Access>
[[nodiscard]] kb::Result<void> StartMeasurement(Access& io, ::sfm::Gas gas,
                                                kb::Milliseconds timeout,
                                                uint16_t o2_permille = 0U) noexcept {
  Command c{};
  c.op = Op::StartMeasurement;
  c.gas = gas;
  c.value = o2_permille;
  return Send(io, c, timeout);
}

/** @brief Stops the measurement. */
template <typename Access>
[[nodiscard]] kb::Result<void> StopMeasurement(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::StopMeasurement;
  return Send(io, c, timeout);
}

/** @brief Sets the averaging (idle only): 0, 1 to 128, or `sfm::kAveragingDefault`. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetAveraging(Access& io, uint16_t n,
                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetAveraging;
  c.value = n;
  return Send(io, c, timeout);
}

/** @brief Sleep (`asleep` true, idle only) or wake up. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetSleep(Access& io, bool asleep,
                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = asleep ? Op::Sleep : Op::WakeUp;
  return Send(io, c, timeout);
}

/** @brief One sample read now (not the snapshot). */
template <typename Access>
[[nodiscard]] kb::Result<::sfm::Sample> ReadSample(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadSample;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::sfm::Sample>::Success(r.Value().sample)
                  : Forward<::sfm::Sample>(r);
}

/** @brief The temperature by the pointer sequence, milli-degrees Celsius (parts that offer it). */
template <typename Access>
[[nodiscard]] kb::Result<int32_t> ReadTemperature(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadTemperature;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<int32_t>::Success(r.Value().temperature_mdegc) : Forward<int32_t>(r);
}

/// @}

} // namespace kb::handlers::sfm_flow_meter

#endif // KB_HANDLERS_BINDINGS_SFM_FLOW_METER_HPP
