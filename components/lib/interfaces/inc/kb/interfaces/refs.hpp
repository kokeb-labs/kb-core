/**
 * @file refs.hpp
 * @brief The type-erased `*Ref` values of every kb::iface contract (ADR-0006 rule 5), in one
 * header.
 *
 * A `*Ref` is a non-owning value: a context pointer and a pointer to a constant table of function
 * pointers, made from any type that satisfies the contract and itself `static_assert`ed against it.
 * It is how a HAL chooses an implementation at run time without a virtual function or a heap.
 *
 * This header is kb-core's one recorded library deviation, DEV-KBC-004 (MISRA C++:2023 Rule 8.2.6):
 * each table entry converts the `void*` context back to the type the table was built for. The
 * conversion is confined here: the context and the table are set together by one constructor
 * template constrained by the concept, and each constructor `static_assert`s that the table it
 * takes was generated for the same type, so a context can only reach the functions of its own type.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_REFS_HPP
#define KB_INTERFACES_REFS_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "kb/interfaces/adc.hpp"
#include "kb/interfaces/can.hpp"
#include "kb/interfaces/gpio.hpp"
#include "kb/interfaces/i2c.hpp"
#include "kb/interfaces/log.hpp"
#include "kb/interfaces/pwm.hpp"
#include "kb/interfaces/spi.hpp"
#include "kb/interfaces/time.hpp"
#include "kb/interfaces/types.hpp"
#include "kb/interfaces/uart.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface {

namespace detail {

/**
 * @brief A *Ref's context back as the type its function table was built for: the one conversion of
 * DEV-KBC-004.
 * @tparam T The implementation type; only a table generated for T calls this with a context made
 * from a T.
 */
template <typename T>
[[nodiscard]] T& ContextAs(void* self) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-004)
  return *static_cast<T*>(self);
}

/** @brief The const form of ContextAs, for the const members (LastFailure, FullScale, Now). */
template <typename T>
[[nodiscard]] const T& ContextAs(const void* self) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-004)
  return *static_cast<const T*>(self);
}

} // namespace detail

// --- I2C
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind an I2cBusRef. */
struct I2cBusOps {
  kb::Status (*ensure_ready)(void* self) noexcept; ///< EnsureReady.
  kb::Status (*write)(void* self, uint8_t address,
                      std::span<const uint8_t> tx) noexcept; ///< Write.
  kb::Status (*write_read)(void* self, uint8_t address, std::span<const uint8_t> tx,
                           std::span<uint8_t> rx) noexcept;                        ///< WriteRead.
  kb::Status (*read)(void* self, uint8_t address, std::span<uint8_t> rx) noexcept; ///< Read.
  I2cFailure (*last_failure)(const void* self) noexcept;                           ///< LastFailure.
};

namespace detail {
/// The I2cBusOps of an implementation type T.
template <I2cBus T>
inline constexpr I2cBusOps kI2cBusOps{
    .ensure_ready = [](void* self) noexcept { return detail::ContextAs<T>(self).EnsureReady(); },
    .write =
        [](void* self, uint8_t address, std::span<const uint8_t> tx) noexcept {
          return detail::ContextAs<T>(self).Write(address, tx);
        },
    .write_read =
        [](void* self, uint8_t address, std::span<const uint8_t> tx,
           std::span<uint8_t> rx) noexcept {
          return detail::ContextAs<T>(self).WriteRead(address, tx, rx);
        },
    .read =
        [](void* self, uint8_t address, std::span<uint8_t> rx) noexcept {
          return detail::ContextAs<T>(self).Read(address, rx);
        },
    .last_failure =
        [](const void* self) noexcept { return detail::ContextAs<T>(self).LastFailure(); },
};
} // namespace detail

/**
 * @brief A non-owning reference to any I2cBus, chosen at run time (a context pointer and a constant
 * function table; no virtual function, no heap). The referenced bus must outlive the reference.
 */
class I2cBusRef {
public:
  /** @brief The referenced object (copies of a I2cBusRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `bus`. */
  template <I2cBus T>
    requires(!std::same_as<std::remove_cvref_t<T>, I2cBusRef>)
  constexpr explicit I2cBusRef(T& bus) noexcept : self_(&bus), ops_(&detail::kI2cBusOps<T>) {
    static_assert(I2cBus<T>, "the table and the context are made for the same type");
  }
  /** @brief I2cBus::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ops_->ensure_ready(self_);
  }
  /** @brief I2cBus::Write. */
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    return ops_->write(self_, address, tx);
  }
  /** @brief I2cBus::WriteRead. */
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    return ops_->write_read(self_, address, tx, rx);
  }
  /** @brief I2cBus::Read. */
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    return ops_->read(self_, address, rx);
  }
  /** @brief I2cBus::LastFailure. */
  [[nodiscard]] I2cFailure LastFailure() const noexcept {
    return ops_->last_failure(self_);
  }

private:
  void* self_;
  const I2cBusOps* ops_;
};

static_assert(I2cBus<I2cBusRef>, "I2cBusRef satisfies the contract it erases");

// --- SPI
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind an SpiDeviceRef. */
struct SpiDeviceOps {
  kb::Status (*ensure_ready)(void* self) noexcept; ///< EnsureReady.
  kb::Status (*transfer)(void* self, std::span<const uint8_t> tx,
                         std::span<uint8_t> rx) noexcept; ///< Transfer.
};

namespace detail {
/// The SpiDeviceOps of an implementation type T.
template <SpiDevice T>
inline constexpr SpiDeviceOps kSpiDeviceOps{
    .ensure_ready = [](void* self) noexcept { return detail::ContextAs<T>(self).EnsureReady(); },
    .transfer =
        [](void* self, std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
          return detail::ContextAs<T>(self).Transfer(tx, rx);
        },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any SpiDevice (context pointer and function
 * table). */
class SpiDeviceRef {
public:
  /** @brief The referenced object (copies of a SpiDeviceRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `device`, which must outlive the reference. */
  template <SpiDevice T>
    requires(!std::same_as<std::remove_cvref_t<T>, SpiDeviceRef>)
  constexpr explicit SpiDeviceRef(T& device) noexcept
      : self_(&device), ops_(&detail::kSpiDeviceOps<T>) {
    static_assert(SpiDevice<T>, "the table and the context are made for the same type");
  }
  /** @brief SpiDevice::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ops_->ensure_ready(self_);
  }
  /** @brief SpiDevice::Transfer. */
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    return ops_->transfer(self_, tx, rx);
  }

private:
  void* self_;
  const SpiDeviceOps* ops_;
};

static_assert(SpiDevice<SpiDeviceRef>, "SpiDeviceRef satisfies the contract it erases");

// --- UART
// --------------------------------------------------------------------------------------------------
/** @brief The function table behind a UartPortRef. */
struct UartPortOps {
  kb::Status (*ensure_ready)(void* self) noexcept;                       ///< EnsureReady.
  kb::Status (*write)(void* self, std::span<const uint8_t> tx) noexcept; ///< Write.
  kb::Result<std::size_t> (*read)(void* self, std::span<uint8_t> rx,
                                  kb::Milliseconds t) noexcept; ///< Read.
  kb::Status (*flush_input)(void* self) noexcept;               ///< FlushInput.
};

namespace detail {
/// The UartPortOps of an implementation type T.
template <UartPort T>
inline constexpr UartPortOps kUartPortOps{
    .ensure_ready = [](void* self) noexcept { return detail::ContextAs<T>(self).EnsureReady(); },
    .write =
        [](void* self, std::span<const uint8_t> tx) noexcept {
          return detail::ContextAs<T>(self).Write(tx);
        },
    .read = [](void* self, std::span<uint8_t> rx,
               kb::Milliseconds t) noexcept { return detail::ContextAs<T>(self).Read(rx, t); },
    .flush_input = [](void* self) noexcept { return detail::ContextAs<T>(self).FlushInput(); },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any UartPort. */
class UartPortRef {
public:
  /** @brief The referenced object (copies of a UartPortRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `port`, which must outlive the reference. */
  template <UartPort T>
    requires(!std::same_as<std::remove_cvref_t<T>, UartPortRef>)
  constexpr explicit UartPortRef(T& port) noexcept : self_(&port), ops_(&detail::kUartPortOps<T>) {
    static_assert(UartPort<T>, "the table and the context are made for the same type");
  }
  /** @brief UartPort::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ops_->ensure_ready(self_);
  }
  /** @brief UartPort::Write. */
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    return ops_->write(self_, tx);
  }
  /** @brief UartPort::Read. */
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    return ops_->read(self_, rx, timeout);
  }
  /** @brief UartPort::FlushInput. */
  [[nodiscard]] kb::Status FlushInput() noexcept {
    return ops_->flush_input(self_);
  }

private:
  void* self_;
  const UartPortOps* ops_;
};

static_assert(UartPort<UartPortRef>, "UartPortRef satisfies the contract it erases");

// --- GPIO
// --------------------------------------------------------------------------------------------------
/** @brief The function table behind a GpioOutputRef. */
struct GpioOutputOps {
  kb::Status (*set)(void* self, Level level) noexcept; ///< Set.
  kb::Status (*toggle)(void* self) noexcept;           ///< Toggle.
};

/** @brief The function table behind a GpioInputRef. */
struct GpioInputOps {
  kb::Status (*get)(void* self, Level& out) noexcept; ///< Get.
};

namespace detail {
/// The GpioOutputOps of an implementation type T.
template <GpioOutput T>
inline constexpr GpioOutputOps kGpioOutputOps{
    .set = [](void* self, Level level) noexcept { return detail::ContextAs<T>(self).Set(level); },
    .toggle = [](void* self) noexcept { return detail::ContextAs<T>(self).Toggle(); },
};
/// The GpioInputOps of an implementation type T.
template <GpioInput T>
inline constexpr GpioInputOps kGpioInputOps{
    .get = [](void* self, Level& out) noexcept { return detail::ContextAs<T>(self).Get(out); },
};
} // namespace detail

/**
 * @brief A non-owning, run-time chosen reference to any GpioOutput: one board can mix an MCU pin
 * and an I/O-expander pin behind the same type.
 */
class GpioOutputRef {
public:
  /** @brief The referenced object (copies of a GpioOutputRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `pin`, which must outlive the reference. */
  template <GpioOutput T>
    requires(!std::same_as<std::remove_cvref_t<T>, GpioOutputRef>)
  constexpr explicit GpioOutputRef(T& pin) noexcept
      : self_(&pin), ops_(&detail::kGpioOutputOps<T>) {
    static_assert(GpioOutput<T>, "the table and the context are made for the same type");
  }
  /** @brief GpioOutput::Set. */
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    return ops_->set(self_, level);
  }
  /** @brief GpioOutput::Toggle. */
  [[nodiscard]] kb::Status Toggle() noexcept {
    return ops_->toggle(self_);
  }

private:
  void* self_;
  const GpioOutputOps* ops_;
};

/** @brief A non-owning, run-time chosen reference to any GpioInput. */
class GpioInputRef {
public:
  /** @brief The referenced object (copies of a GpioInputRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `pin`, which must outlive the reference. */
  template <GpioInput T>
    requires(!std::same_as<std::remove_cvref_t<T>, GpioInputRef>)
  constexpr explicit GpioInputRef(T& pin) noexcept : self_(&pin), ops_(&detail::kGpioInputOps<T>) {
    static_assert(GpioInput<T>, "the table and the context are made for the same type");
  }
  /** @brief GpioInput::Get. */
  [[nodiscard]] kb::Status Get(Level& out) noexcept {
    return ops_->get(self_, out);
  }

private:
  void* self_;
  const GpioInputOps* ops_;
};

static_assert(GpioOutput<GpioOutputRef>, "GpioOutputRef satisfies the contract it erases");
static_assert(GpioInput<GpioInputRef>, "GpioInputRef satisfies the contract it erases");

// --- ADC
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind an AdcChannelRef. */
struct AdcChannelOps {
  kb::Result<uint32_t> (*read_raw)(void* self) noexcept;       ///< ReadRaw.
  kb::Result<int32_t> (*read_millivolts)(void* self) noexcept; ///< ReadMillivolts.
  uint32_t (*full_scale)(const void* self) noexcept;           ///< FullScale.
};

namespace detail {
/// The AdcChannelOps of an implementation type T.
template <AdcChannel T>
inline constexpr AdcChannelOps kAdcChannelOps{
    .read_raw = [](void* self) noexcept { return detail::ContextAs<T>(self).ReadRaw(); },
    .read_millivolts =
        [](void* self) noexcept { return detail::ContextAs<T>(self).ReadMillivolts(); },
    .full_scale = [](const void* self) noexcept { return detail::ContextAs<T>(self).FullScale(); },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any AdcChannel. */
class AdcChannelRef {
public:
  /** @brief The referenced object (copies of a AdcChannelRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `adc`, which must outlive the reference. */
  template <AdcChannel T>
    requires(!std::same_as<std::remove_cvref_t<T>, AdcChannelRef>)
  constexpr explicit AdcChannelRef(T& adc) noexcept
      : self_(&adc), ops_(&detail::kAdcChannelOps<T>) {
    static_assert(AdcChannel<T>, "the table and the context are made for the same type");
  }
  /** @brief AdcChannel::ReadRaw. */
  [[nodiscard]] kb::Result<uint32_t> ReadRaw() noexcept {
    return ops_->read_raw(self_);
  }
  /** @brief AdcChannel::ReadMillivolts. */
  [[nodiscard]] kb::Result<int32_t> ReadMillivolts() noexcept {
    return ops_->read_millivolts(self_);
  }
  /** @brief AdcChannel::FullScale. */
  [[nodiscard]] uint32_t FullScale() const noexcept {
    return ops_->full_scale(self_);
  }

private:
  void* self_;
  const AdcChannelOps* ops_;
};

static_assert(AdcChannel<AdcChannelRef>, "AdcChannelRef satisfies the contract it erases");

// --- PWM
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind a PwmOutputRef. */
struct PwmOutputOps {
  kb::Status (*set_frequency_hz)(void* self, uint32_t hz) noexcept; ///< SetFrequencyHz.
  kb::Status (*set_duty)(void* self, Duty duty) noexcept;           ///< SetDuty.
  kb::Status (*enable)(void* self, bool on) noexcept;               ///< Enable.
};

namespace detail {
/// The PwmOutputOps of an implementation type T.
template <PwmOutput T>
inline constexpr PwmOutputOps kPwmOutputOps{
    .set_frequency_hz =
        [](void* self, uint32_t hz) noexcept {
          return detail::ContextAs<T>(self).SetFrequencyHz(hz);
        },
    .set_duty = [](void* self,
                   Duty duty) noexcept { return detail::ContextAs<T>(self).SetDuty(duty); },
    .enable = [](void* self, bool on) noexcept { return detail::ContextAs<T>(self).Enable(on); },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any PwmOutput. */
class PwmOutputRef {
public:
  /** @brief The referenced object (copies of a PwmOutputRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `pwm`, which must outlive the reference. */
  template <PwmOutput T>
    requires(!std::same_as<std::remove_cvref_t<T>, PwmOutputRef>)
  constexpr explicit PwmOutputRef(T& pwm) noexcept : self_(&pwm), ops_(&detail::kPwmOutputOps<T>) {
    static_assert(PwmOutput<T>, "the table and the context are made for the same type");
  }
  /** @brief PwmOutput::SetFrequencyHz. */
  [[nodiscard]] kb::Status SetFrequencyHz(uint32_t hz) noexcept {
    return ops_->set_frequency_hz(self_, hz);
  }
  /** @brief PwmOutput::SetDuty. */
  [[nodiscard]] kb::Status SetDuty(Duty duty) noexcept {
    return ops_->set_duty(self_, duty);
  }
  /** @brief PwmOutput::Enable. */
  [[nodiscard]] kb::Status Enable(bool on) noexcept {
    return ops_->enable(self_, on);
  }

private:
  void* self_;
  const PwmOutputOps* ops_;
};

static_assert(PwmOutput<PwmOutputRef>, "PwmOutputRef satisfies the contract it erases");

// --- CAN
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind a CanBusRef. */
struct CanBusOps {
  kb::Status (*ensure_ready)(void* self) noexcept; ///< EnsureReady.
  kb::Status (*send)(void* self, const CanFrame& frame, kb::Milliseconds t) noexcept; ///< Send.
  kb::Status (*receive)(void* self, CanFrame& frame, kb::Milliseconds t) noexcept;    ///< Receive.
};

namespace detail {
/// The CanBusOps of an implementation type T.
template <CanBus T>
inline constexpr CanBusOps kCanBusOps{
    .ensure_ready = [](void* self) noexcept { return detail::ContextAs<T>(self).EnsureReady(); },
    .send = [](void* self, const CanFrame& frame,
               kb::Milliseconds t) noexcept { return detail::ContextAs<T>(self).Send(frame, t); },
    .receive =
        [](void* self, CanFrame& frame, kb::Milliseconds t) noexcept {
          return detail::ContextAs<T>(self).Receive(frame, t);
        },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any CanBus. */
class CanBusRef {
public:
  /** @brief The referenced object (copies of a CanBusRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `can`, which must outlive the reference. */
  template <CanBus T>
    requires(!std::same_as<std::remove_cvref_t<T>, CanBusRef>)
  constexpr explicit CanBusRef(T& can) noexcept : self_(&can), ops_(&detail::kCanBusOps<T>) {
    static_assert(CanBus<T>, "the table and the context are made for the same type");
  }
  /** @brief CanBus::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ops_->ensure_ready(self_);
  }
  /** @brief CanBus::Send. */
  [[nodiscard]] kb::Status Send(const CanFrame& frame, kb::Milliseconds timeout) noexcept {
    return ops_->send(self_, frame, timeout);
  }
  /** @brief CanBus::Receive. */
  [[nodiscard]] kb::Status Receive(CanFrame& frame, kb::Milliseconds timeout) noexcept {
    return ops_->receive(self_, frame, timeout);
  }

private:
  void* self_;
  const CanBusOps* ops_;
};

static_assert(CanBus<CanBusRef>, "CanBusRef satisfies the contract it erases");

// --- TIME
// --------------------------------------------------------------------------------------------------
/** @brief The function table behind a MonotonicClockRef. */
struct MonotonicClockOps {
  kb::Instant (*now)(const void* self) noexcept; ///< Now.
};

/** @brief The function table behind a BusyWaitRef. */
struct BusyWaitOps {
  void (*wait_us)(void* self, kb::Microseconds us) noexcept; ///< WaitUs.
};

namespace detail {
/// The MonotonicClockOps of an implementation type T.
template <MonotonicClock T>
inline constexpr MonotonicClockOps kMonotonicClockOps{
    [](const void* self) noexcept { return detail::ContextAs<T>(self).Now(); },
};
/// The BusyWaitOps of an implementation type T.
template <BusyWait T>
inline constexpr BusyWaitOps kBusyWaitOps{
    [](void* self, kb::Microseconds us) noexcept { detail::ContextAs<T>(self).WaitUs(us); },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any MonotonicClock. */
class MonotonicClockRef {
public:
  /** @brief The referenced object (copies of a MonotonicClockRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `clock`, which must outlive the reference. */
  template <MonotonicClock T>
    requires(!std::same_as<std::remove_cvref_t<T>, MonotonicClockRef>)
  constexpr explicit MonotonicClockRef(const T& clock) noexcept
      : self_(&clock), ops_(&detail::kMonotonicClockOps<T>) {
    static_assert(MonotonicClock<T>, "the table and the context are made for the same type");
  }
  /** @brief MonotonicClock::Now. */
  [[nodiscard]] kb::Instant Now() const noexcept {
    return ops_->now(self_);
  }

private:
  const void* self_;
  const MonotonicClockOps* ops_;
};

/** @brief A non-owning, run-time chosen reference to any BusyWait. */
class BusyWaitRef {
public:
  /** @brief The referenced object (copies of a BusyWaitRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `delay`, which must outlive the reference. */
  template <BusyWait T>
    requires(!std::same_as<std::remove_cvref_t<T>, BusyWaitRef>)
  constexpr explicit BusyWaitRef(T& delay) noexcept
      : self_(&delay), ops_(&detail::kBusyWaitOps<T>) {
    static_assert(BusyWait<T>, "the table and the context are made for the same type");
  }
  /** @brief BusyWait::WaitUs. */
  void WaitUs(kb::Microseconds us) noexcept {
    ops_->wait_us(self_, us);
  }

private:
  void* self_;
  const BusyWaitOps* ops_;
};

static_assert(MonotonicClock<MonotonicClockRef>,
              "MonotonicClockRef satisfies the contract it erases");
static_assert(BusyWait<BusyWaitRef>, "BusyWaitRef satisfies the contract it erases");

// --- LOG
// ---------------------------------------------------------------------------------------------------
/** @brief The function table behind a LogSinkRef. */
struct LogSinkOps {
  void (*write)(void* self, LogLevel level, std::string_view text) noexcept; ///< Write.
};

namespace detail {
/// The LogSinkOps of an implementation type T.
template <LogSink T>
inline constexpr LogSinkOps kLogSinkOps{
    [](void* self, LogLevel level, std::string_view text) noexcept {
      detail::ContextAs<T>(self).Write(level, text);
    },
};
} // namespace detail

/** @brief A non-owning, run-time chosen reference to any LogSink. */
class LogSinkRef {
public:
  /** @brief The referenced object (copies of a LogSinkRef refer to the same one). */
  [[nodiscard]] const void* Context() const noexcept {
    return self_;
  }
  /** @brief Refers to `sink`, which must outlive the reference. */
  template <LogSink T>
    requires(!std::same_as<std::remove_cvref_t<T>, LogSinkRef>)
  constexpr explicit LogSinkRef(T& sink) noexcept : self_(&sink), ops_(&detail::kLogSinkOps<T>) {
    static_assert(LogSink<T>, "the table and the context are made for the same type");
  }
  /** @brief LogSink::Write. */
  void Write(LogLevel level, std::string_view text) noexcept {
    ops_->write(self_, level, text);
  }

private:
  void* self_;
  const LogSinkOps* ops_;
};

static_assert(LogSink<LogSinkRef>, "LogSinkRef satisfies the contract it erases");

static_assert(
    std::is_trivially_copyable_v<I2cBusRef> && std::is_trivially_copyable_v<SpiDeviceRef> &&
        std::is_trivially_copyable_v<UartPortRef> && std::is_trivially_copyable_v<GpioOutputRef> &&
        std::is_trivially_copyable_v<GpioInputRef> && std::is_trivially_copyable_v<AdcChannelRef> &&
        std::is_trivially_copyable_v<PwmOutputRef> && std::is_trivially_copyable_v<CanBusRef> &&
        std::is_trivially_copyable_v<MonotonicClockRef> &&
        std::is_trivially_copyable_v<BusyWaitRef> && std::is_trivially_copyable_v<LogSinkRef>,
    "a *Ref is a plain pair of pointers: copying it copies the reference, never wraps it");

} // namespace kb::iface

#endif // KB_INTERFACES_REFS_HPP
