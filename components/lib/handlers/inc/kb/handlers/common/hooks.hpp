/**
 * @file hooks.hpp
 * @brief The CRTP hook bases of the bindings (ADR-0006 register): `I2cHooks`, `SpiHooks`,
 * `UartHooks`, `AdcHooks` and `GpioHooks` implement once, for every chip on that bus, what each
 * driver port hook needs from a kb contract: the transfer with failure recording and counters, the
 * at-least delay split (busy-wait below 1 ms, kb::rtos::SleepFor from 1 ms) and clock narrowing to
 * the 32-bit clocks drivers take (wrap semantics: drivers compare by unsigned elapsed time).
 *
 * A binding's port derives from the driver's own CRTP port base and from one of these, and writes
 * each driver hook as a one-line forward:
 *
 * @code
 * template <kb::iface::I2cBus Bus>
 * class Pcal9555Port : public pcal9555::I2cInterface<Pcal9555Port<Bus>>,
 *                      public kb::handlers::I2cHooks<Pcal9555Port<Bus>, Bus> {
 *  public:
 *   bool EnsureInitialized() noexcept { return this->BusReady(); }
 *   bool Write(uint8_t a, const uint8_t* d, std::size_t n) noexcept { return this->BusWrite(a, d,
 * n); }
 *   ...
 * };
 * @endcode
 *
 * A derived port may declare `void OnBusFailure(kb::Status status) noexcept` to see every failed
 * transfer (it is called after the counters are updated).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_HOOKS_HPP
#define KB_HANDLERS_COMMON_HOOKS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/common/health.hpp"
#include "kb/interfaces/interfaces.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief Waits below this many microseconds spin; longer ones sleep (the at-least split). */
inline constexpr uint32_t kSpinBelowUs = 1000U;

/** @brief The clock and the spin delay every hook base uses (type-erased, chosen by the HAL). */
class HookTiming {
public:
  /**
   * @brief Uses `clock` for time and `spin` for waits below kSpinBelowUs. Both are taken as const
   * references so that the copy constructor of each *Ref is chosen (a non-const *Ref lvalue would
   * match the *Ref's own converting constructor and refer to the parameter).
   */
  constexpr HookTiming(const kb::iface::MonotonicClockRef& clock,
                       const kb::iface::BusyWaitRef& spin) noexcept
      : clock_(clock), spin_(spin) {}

  /** @brief Waits at least `us` microseconds: a spin below 1 ms, a sleep (rounded up) from 1 ms. */
  void DelayUs(uint32_t us) noexcept {
    if (us < kSpinBelowUs) {
      spin_.WaitUs(kb::Microseconds{us});
    } else {
      kb::rtos::SleepFor(kb::Microseconds{us});
    }
  }

  /** @brief Waits at least `ms` milliseconds (a sleep; 0 yields). */
  static void DelayMs(uint32_t ms) noexcept {
    kb::rtos::SleepFor(kb::Milliseconds{ms});
  }

  /** @brief The 64-bit clock. */
  [[nodiscard]] kb::Instant Now() const noexcept {
    return clock_.Now();
  }

  /** @brief The low 32 bits of the microsecond clock (wraps after about 71.6 minutes). */
  [[nodiscard]] uint32_t NowUs32() const noexcept {
    return kb::utils::NarrowUs(clock_.Now());
  }

  /** @brief The low 32 bits of the millisecond clock (wraps after about 49.7 days). */
  [[nodiscard]] uint32_t NowMs32() const noexcept {
    return kb::utils::NarrowMs(clock_.Now());
  }

private:
  kb::iface::MonotonicClockRef clock_;
  kb::iface::BusyWaitRef spin_;
};

/** @brief Counters every hook base keeps. */
struct HookCounters {
  uint32_t transfers{0U};                 ///< Bus operations attempted.
  uint32_t failures{0U};                  ///< Bus operations that failed.
  kb::Status last_status{kb::Status::Ok}; ///< The last failure; Ok when none.
};

/**
 * @brief What every hook base shares: the counters, the timing and the failure callback.
 * Not a base class of its own: each hook base holds one (composition).
 */
template <typename Derived>
class HookRecorder {
public:
  /** @brief Records `status` of one bus operation of `port`; true when it is Ok. */
  [[nodiscard]] bool Record(Derived& port, kb::Status status) noexcept {
    static_cast<void>(port); // used only in one branch of the `if constexpr` below
    counters_.transfers = SaturatingIncrement(counters_.transfers);
    const bool ok = status == kb::Status::Ok;
    if (!ok) {
      counters_.failures = SaturatingIncrement(counters_.failures);
      counters_.last_status = status;
      if constexpr (requires(Derived& d) {
                      { d.OnBusFailure(status) } noexcept;
                    }) {
        port.OnBusFailure(status);
      }
    }
    return ok;
  }

  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return counters_;
  }

private:
  HookCounters counters_{};
};

/**
 * @brief Hooks over a kb::iface::I2cBus.
 * @tparam Derived The binding's port.
 * @tparam Bus The bus type (an I2cBus or I2cBusRef).
 */
template <typename Derived, kb::iface::I2cBus Bus>
class I2cHooks {
public:
  I2cHooks(const I2cHooks&) = delete;
  I2cHooks& operator=(const I2cHooks&) = delete;
  I2cHooks(I2cHooks&&) = delete;
  I2cHooks& operator=(I2cHooks&&) = delete;
  /** @brief EnsureReady() of the bus. */
  [[nodiscard]] bool BusReady() noexcept {
    return recorder_.Record(self(), bus_.EnsureReady());
  }
  /** @brief One write transfer of `len` bytes at `data` to `address`. */
  [[nodiscard]] bool BusWrite(uint8_t address, const uint8_t* data, std::size_t len) noexcept {
    kb::Status s = kb::Status::InvalidArgument;
    if ((data != nullptr) || (len == 0U)) {
      s = bus_.Write(address, std::span<const uint8_t>{data, len});
    }
    return recorder_.Record(self(), s);
  }
  /** @brief Write `tx` then read `rx` with a repeated START. */
  [[nodiscard]] bool BusWriteRead(uint8_t address, const uint8_t* tx, std::size_t tx_len,
                                  uint8_t* rx, std::size_t rx_len) noexcept {
    kb::Status s = kb::Status::InvalidArgument;
    if (((tx != nullptr) || (tx_len == 0U)) && ((rx != nullptr) || (rx_len == 0U))) {
      s = bus_.WriteRead(address, std::span<const uint8_t>{tx, tx_len},
                         std::span<uint8_t>{rx, rx_len});
    }
    return recorder_.Record(self(), s);
  }
  /** @brief One read transfer of `len` bytes into `rx`. */
  [[nodiscard]] bool BusRead(uint8_t address, uint8_t* rx, std::size_t len) noexcept {
    kb::Status s = kb::Status::InvalidArgument;
    if ((rx != nullptr) || (len == 0U)) {
      s = bus_.Read(address, std::span<uint8_t>{rx, len});
    }
    return recorder_.Record(self(), s);
  }
  /** @brief Why the last transfer failed. */
  [[nodiscard]] kb::iface::I2cFailure BusLastFailure() const noexcept {
    return bus_.LastFailure();
  }
  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }
  /** @brief The timing (delays and narrowed clocks). */
  [[nodiscard]] HookTiming& Timing() noexcept {
    return timing_;
  }

protected:
  /** @brief Over `bus` with `timing`; no I/O. */
  I2cHooks(Bus& bus, const HookTiming& timing) noexcept : bus_(bus), timing_(timing) {}
  ~I2cHooks() = default;

private:
  [[nodiscard]] Derived& self() noexcept {
    return static_cast<Derived&>(*this);
  }
  Bus& bus_;
  HookTiming timing_;
  HookRecorder<Derived> recorder_{};
};

/**
 * @brief Hooks over a kb::iface::SpiDevice (one chip select).
 * @tparam Derived The binding's port.
 * @tparam Dev The device type (a SpiDevice or SpiDeviceRef).
 */
template <typename Derived, kb::iface::SpiDevice Dev>
class SpiHooks {
public:
  SpiHooks(const SpiHooks&) = delete;
  SpiHooks& operator=(const SpiHooks&) = delete;
  SpiHooks(SpiHooks&&) = delete;
  SpiHooks& operator=(SpiHooks&&) = delete;
  /** @brief EnsureReady() of the device. */
  [[nodiscard]] bool BusReady() noexcept {
    return recorder_.Record(self(), dev_.EnsureReady());
  }
  /** @brief One full-duplex frame of `len` bytes (`tx` or `rx` may be null: write or read only). */
  [[nodiscard]] bool BusTransfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    const std::span<const uint8_t> out =
        (tx != nullptr) ? std::span<const uint8_t>{tx, len} : std::span<const uint8_t>{};
    const std::span<uint8_t> in =
        (rx != nullptr) ? std::span<uint8_t>{rx, len} : std::span<uint8_t>{};
    const bool some = (tx != nullptr) || (rx != nullptr) || (len == 0U);
    const kb::Status s = some ? dev_.Transfer(out, in) : kb::Status::InvalidArgument;
    return recorder_.Record(self(), s);
  }
  /** @brief Changes the clock where the device allows it; Unsupported otherwise. */
  [[nodiscard]] bool BusSetClock(uint32_t hz) noexcept {
    static_cast<void>(hz); // used only in one branch of the `if constexpr` below
    kb::Status s = kb::Status::Unsupported;
    if constexpr (kb::iface::SpiClockControl<Dev>) {
      s = dev_.SetClockHz(hz);
    }
    return recorder_.Record(self(), s);
  }
  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }
  /** @brief The timing (delays and narrowed clocks). */
  [[nodiscard]] HookTiming& Timing() noexcept {
    return timing_;
  }

protected:
  /** @brief Over `dev` with `timing`; no I/O. */
  SpiHooks(Dev& dev, const HookTiming& timing) noexcept : dev_(dev), timing_(timing) {}
  ~SpiHooks() = default;

private:
  [[nodiscard]] Derived& self() noexcept {
    return static_cast<Derived&>(*this);
  }
  Dev& dev_;
  HookTiming timing_;
  HookRecorder<Derived> recorder_{};
};

/**
 * @brief Hooks over a kb::iface::UartPort.
 * @tparam Derived The binding's port.
 * @tparam Port The port type (a UartPort or UartPortRef).
 */
template <typename Derived, kb::iface::UartPort Port>
class UartHooks {
public:
  UartHooks(const UartHooks&) = delete;
  UartHooks& operator=(const UartHooks&) = delete;
  UartHooks(UartHooks&&) = delete;
  UartHooks& operator=(UartHooks&&) = delete;
  /** @brief EnsureReady() of the port. */
  [[nodiscard]] bool BusReady() noexcept {
    return recorder_.Record(self(), port_.EnsureReady());
  }
  /** @brief Queues `len` bytes at `data`. */
  [[nodiscard]] bool BusWrite(const uint8_t* data, std::size_t len) noexcept {
    kb::Status s = kb::Status::InvalidArgument;
    if ((data != nullptr) || (len == 0U)) {
      s = port_.Write(std::span<const uint8_t>{data, len});
    }
    return recorder_.Record(self(), s);
  }
  /**
   * @brief Reads into `rx` until `len` bytes arrived or `timeout_ms` passed since the call.
   * @return The bytes received (fewer than `len` on a timeout, which is not counted as a failure).
   */
  [[nodiscard]] std::size_t BusRead(uint8_t* rx, std::size_t len, uint32_t timeout_ms) noexcept {
    std::size_t got = 0U;
    bool failed = (rx == nullptr) && (len != 0U);
    const std::span<uint8_t> all = failed ? std::span<uint8_t>{} : std::span<uint8_t>{rx, len};
    const kb::utils::Deadline deadline =
        kb::utils::Deadline::After(timing_.Now(), kb::Milliseconds{timeout_ms});
    bool more = (!failed) && (len != 0U);
    while (more) {
      const uint64_t left_us = deadline.RemainingUs(timing_.Now());
      const kb::Microseconds left{
          static_cast<uint32_t>((left_us > kMaxWaitUs) ? kMaxWaitUs : left_us)};
      const kb::Result<std::size_t> r =
          port_.Read(all.subspan(got), kb::utils::ToMillisecondsCeil(left));
      failed = (!r.IsOk()) && (r.Error() != kb::Status::Timeout);
      got += r.IsOk() ? r.Value() : 0U;
      more = (!failed) && (got < len) && (left_us != 0U);
    }
    static_cast<void>(recorder_.Record(self(), failed ? kb::Status::BusError : kb::Status::Ok));
    return got;
  }
  /**
   * @brief Reads what has arrived into `rx` (at most `len` bytes), waiting at most `timeout_ms` for
   * the first byte. @return The bytes received; 0 on a timeout (not counted as a failure).
   */
  [[nodiscard]] std::size_t BusReadSome(uint8_t* rx, std::size_t len,
                                        uint32_t timeout_ms) noexcept {
    std::size_t got = 0U;
    kb::Status s = kb::Status::InvalidArgument;
    if ((rx != nullptr) || (len == 0U)) {
      const kb::Result<std::size_t> r =
          port_.Read(std::span<uint8_t>{rx, len}, kb::Milliseconds{timeout_ms});
      got = r.IsOk() ? r.Value() : 0U;
      s = (r.IsOk() || (r.Error() == kb::Status::Timeout)) ? kb::Status::Ok : r.Error();
    }
    static_cast<void>(recorder_.Record(self(), s));
    return got;
  }
  /** @brief Drops what was received and not read. */
  [[nodiscard]] bool BusFlush() noexcept {
    return recorder_.Record(self(), port_.FlushInput());
  }
  /** @brief Flush, write `tx`, then read exactly `rx_len` bytes within `timeout_ms`. */
  [[nodiscard]] bool BusExchange(const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                                 std::size_t rx_len, uint32_t timeout_ms) noexcept {
    bool ok = BusFlush();
    if (ok) {
      ok = BusWrite(tx, tx_len);
    }
    if (ok) {
      ok = BusRead(rx, rx_len, timeout_ms) == rx_len;
    }
    return ok;
  }
  /** @brief Changes the baud rate where the port allows it. */
  [[nodiscard]] bool BusSetBaud(uint32_t baud) noexcept {
    static_cast<void>(baud); // used only in one branch of the `if constexpr` below
    kb::Status s = kb::Status::Unsupported;
    if constexpr (kb::iface::UartBaudControl<Port>) {
      s = port_.SetBaud(baud);
    }
    return recorder_.Record(self(), s);
  }
  /** @brief Waits until the last byte left the wire, where the port can tell; Ok otherwise. */
  [[nodiscard]] bool BusWaitTxDone(uint32_t timeout_ms) noexcept {
    static_cast<void>(timeout_ms); // used only in one branch of the `if constexpr` below
    kb::Status s = kb::Status::Ok;
    if constexpr (kb::iface::UartTxDrain<Port>) {
      s = port_.WaitTxDone(kb::Milliseconds{timeout_ms});
    }
    return recorder_.Record(self(), s);
  }
  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }
  /** @brief The timing (delays and narrowed clocks). */
  [[nodiscard]] HookTiming& Timing() noexcept {
    return timing_;
  }

protected:
  /** @brief Over `port` with `timing`; no I/O. */
  UartHooks(Port& port, const HookTiming& timing) noexcept : port_(port), timing_(timing) {}
  ~UartHooks() = default;

private:
  static constexpr uint64_t kMaxWaitUs = 0xFFFFFFFFU;
  [[nodiscard]] Derived& self() noexcept {
    return static_cast<Derived&>(*this);
  }
  Port& port_;
  HookTiming timing_;
  HookRecorder<Derived> recorder_{};
};

/**
 * @brief Hooks over a kb::iface::AdcChannel.
 * @tparam Derived The binding's port.
 * @tparam Adc The channel type (an AdcChannel or AdcChannelRef).
 */
template <typename Derived, kb::iface::AdcChannel Adc>
class AdcHooks {
public:
  AdcHooks(const AdcHooks&) = delete;
  AdcHooks& operator=(const AdcHooks&) = delete;
  AdcHooks(AdcHooks&&) = delete;
  AdcHooks& operator=(AdcHooks&&) = delete;
  /** @brief One raw conversion into `raw`. */
  [[nodiscard]] bool AdcRaw(uint32_t& raw) noexcept {
    const kb::Result<uint32_t> r = adc_.ReadRaw();
    raw = r.ValueOr(0U);
    return recorder_.Record(self(), r.Error());
  }
  /** @brief One calibrated conversion into `mv`. */
  [[nodiscard]] bool AdcMillivolts(int32_t& mv) noexcept {
    const kb::Result<int32_t> r = adc_.ReadMillivolts();
    mv = r.ValueOr(0);
    return recorder_.Record(self(), r.Error());
  }
  /** @brief The largest raw code. */
  [[nodiscard]] uint32_t AdcFullScale() const noexcept {
    return adc_.FullScale();
  }
  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }
  /** @brief The timing (delays and narrowed clocks). */
  [[nodiscard]] HookTiming& Timing() noexcept {
    return timing_;
  }

protected:
  /** @brief Over `adc` with `timing`; no I/O. */
  AdcHooks(Adc& adc, const HookTiming& timing) noexcept : adc_(adc), timing_(timing) {}
  ~AdcHooks() = default;

private:
  [[nodiscard]] Derived& self() noexcept {
    return static_cast<Derived&>(*this);
  }
  Adc& adc_;
  HookTiming timing_;
  HookRecorder<Derived> recorder_{};
};

/** @brief One control output a HAL may supply: the pin and its polarity (board value). */
struct OutputPin {
  std::optional<kb::iface::GpioOutputRef> pin; ///< Absent: the board does not wire it.
  bool active_low{false};                      ///< True when "asserted" is the low level.
};

/** @brief One control input a HAL may supply: the pin and its polarity (board value). */
struct InputPin {
  std::optional<kb::iface::GpioInputRef> pin; ///< Absent: the board does not wire it.
  bool active_low{false};                     ///< True when "asserted" is the low level.
};

/**
 * @brief Hooks over control pins (reset, enable, chip select, ready, interrupt lines): the
 * driver's `CtrlPin` map, with polarity, taken from HAL configuration.
 * @tparam Derived The binding's port.
 * @tparam Outputs Control outputs (indices the binding names).
 * @tparam Inputs Control inputs.
 */
template <typename Derived, std::size_t Outputs, std::size_t Inputs>
class GpioHooks {
public:
  GpioHooks(const GpioHooks&) = delete;
  GpioHooks& operator=(const GpioHooks&) = delete;
  GpioHooks(GpioHooks&&) = delete;
  GpioHooks& operator=(GpioHooks&&) = delete;
  /** @brief True when the board wires output `index`. */
  [[nodiscard]] bool HasOutput(std::size_t index) const noexcept {
    return (index < Outputs) && outputs_[index].pin.has_value();
  }
  /** @brief True when the board wires input `index`. */
  [[nodiscard]] bool HasInput(std::size_t index) const noexcept {
    return (index < Inputs) && inputs_[index].pin.has_value();
  }
  /** @brief Drives output `index` asserted (`true`) or released; false when absent or failed. */
  [[nodiscard]] bool DriveOutput(std::size_t index, bool asserted) noexcept {
    kb::Status s = kb::Status::Unsupported;
    if (index < Outputs) {
      OutputPin& p = outputs_[index];
      if (p.pin.has_value()) {
        const bool high = asserted != p.active_low;
        s = p.pin->Set(high ? kb::iface::Level::High : kb::iface::Level::Low);
      }
    }
    return recorder_.Record(self(), s);
  }
  /** @brief Reads input `index` into `asserted`; false when absent or failed. */
  [[nodiscard]] bool ReadInput(std::size_t index, bool& asserted) noexcept {
    kb::Status s = kb::Status::Unsupported;
    asserted = false;
    if (index < Inputs) {
      InputPin& p = inputs_[index];
      if (p.pin.has_value()) {
        kb::iface::Level level = kb::iface::Level::Low;
        s = p.pin->Get(level);
        asserted = (s == kb::Status::Ok) && ((level == kb::iface::Level::High) != p.active_low);
      }
    }
    return recorder_.Record(self(), s);
  }
  /** @brief The counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }

protected:
  /** @brief Over the HAL's pins; no I/O. */
  GpioHooks(const std::array<OutputPin, Outputs>& outputs,
            const std::array<InputPin, Inputs>& inputs) noexcept
      : outputs_(outputs), inputs_(inputs) {}
  ~GpioHooks() = default;

private:
  [[nodiscard]] Derived& self() noexcept {
    return static_cast<Derived&>(*this);
  }
  std::array<OutputPin, Outputs> outputs_;
  std::array<InputPin, Inputs> inputs_;
  HookRecorder<Derived> recorder_{};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_HOOKS_HPP
