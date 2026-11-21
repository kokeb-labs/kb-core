/**
 * @file fake_pins.hpp
 * @brief Host fakes of the pin, ADC, PWM, CAN, clock, delay and log contracts, for host tests. Test
 * code, outside the MISRA claim.
 *
 * - FakeGpioPin: output, input and interrupt in one; Drive() changes the level as the outside world
 * would and fires the interrupt on a matching edge.
 * - FakeAdc: a settable raw value, millivolts from a reference; fault injection.
 * - FakePwm: records frequency, duty and enable.
 * - FakeCanBus: a transmit log and a receive queue; bus-off injection.
 * - FakeClock: a settable 64-bit clock, including jumps next to 2^31 and 2^32 microseconds.
 * - FakeBusyWait: adds every wait to a total and, if attached, to a FakeClock.
 * - FakeLogSink: counts lines per level and keeps the last one.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_HOST_FAKE_PINS_HPP
#define KB_INTERFACES_HOST_FAKE_PINS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "kb/interfaces/adc.hpp"
#include "kb/interfaces/can.hpp"
#include "kb/interfaces/gpio.hpp"
#include "kb/interfaces/log.hpp"
#include "kb/interfaces/pwm.hpp"
#include "kb/interfaces/time.hpp"
#include "kb/interfaces/types.hpp"
#include "kb/utils/fixed_string.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/ring_buffer.hpp"
#include "kb/utils/static_vector.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface::host {

/** @brief A pin that is output, input and interrupt source at once. */
class FakeGpioPin {
public:
  /** @brief The level the outside world drives; fires the interrupt on a matching edge. */
  void Drive(Level level) noexcept {
    const Level before = level_;
    level_ = level;
    const bool rising = (before == Level::Low) && (level == Level::High);
    const bool falling = (before == Level::High) && (level == Level::Low);
    const bool match = (rising && (edge_ != Edge::Falling)) || (falling && (edge_ != Edge::Rising));
    if (match && (callback_ != nullptr)) {
      ++interrupts_;
      callback_(context_);
    }
  }
  /** @brief The level now. */
  [[nodiscard]] Level LevelNow() const noexcept {
    return level_;
  }
  /** @brief Set() and Toggle() calls. */
  [[nodiscard]] uint32_t Writes() const noexcept {
    return writes_;
  }
  /** @brief Interrupts fired. */
  [[nodiscard]] uint32_t Interrupts() const noexcept {
    return interrupts_;
  }
  /** @brief The next call fails with `status` (Ok clears). */
  void FailNext(kb::Status status) noexcept {
    fail_ = status;
  }

  /** @brief GpioOutput::Set. */
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    const kb::Status s = take();
    if (s == kb::Status::Ok) {
      level_ = level;
      ++writes_;
    }
    return s;
  }
  /** @brief GpioOutput::Toggle. */
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == Level::Low) ? Level::High : Level::Low);
  }
  /** @brief GpioInput::Get. */
  [[nodiscard]] kb::Status Get(Level& out) noexcept {
    const kb::Status s = take();
    if (s == kb::Status::Ok) {
      out = level_;
    }
    return s;
  }
  /** @brief GpioInterrupt::EnableInterrupt; a null callback is InvalidArgument. */
  [[nodiscard]] kb::Status EnableInterrupt(Edge edge, GpioCallback fn, void* context) noexcept {
    kb::Status s = take();
    if ((s == kb::Status::Ok) && (fn == nullptr)) {
      s = kb::Status::InvalidArgument;
    }
    if (s == kb::Status::Ok) {
      edge_ = edge;
      callback_ = fn;
      context_ = context;
    }
    return s;
  }
  /** @brief GpioInterrupt::DisableInterrupt. */
  [[nodiscard]] kb::Status DisableInterrupt() noexcept {
    callback_ = nullptr;
    return kb::Status::Ok;
  }

private:
  [[nodiscard]] kb::Status take() noexcept {
    const kb::Status s = fail_;
    fail_ = kb::Status::Ok;
    return s;
  }

  Level level_{Level::Low};
  Edge edge_{Edge::Both};
  GpioCallback callback_{nullptr};
  void* context_{nullptr};
  kb::Status fail_{kb::Status::Ok};
  uint32_t writes_{0U};
  uint32_t interrupts_{0U};
};

/** @brief An ADC channel with a settable value. */
class FakeAdc {
public:
  /** @brief A 12-bit channel with a 3300 mV reference. */
  constexpr FakeAdc() noexcept = default;
  /** @brief The raw count the next conversions return (clamped to full scale). */
  void SetRaw(uint32_t raw) noexcept {
    raw_ = (raw > kFullScale) ? kFullScale : raw;
  }
  /** @brief The next conversion fails with `status` (Ok clears). */
  void FailNext(kb::Status status) noexcept {
    fail_ = status;
  }
  /** @brief AdcChannel::ReadRaw. */
  [[nodiscard]] kb::Result<uint32_t> ReadRaw() noexcept {
    const kb::Status s = take();
    return (s == kb::Status::Ok) ? kb::Result<uint32_t>::Success(raw_)
                                 : kb::Result<uint32_t>::Failure(s);
  }
  /** @brief AdcChannel::ReadMillivolts: raw * 3300 / 4095, rounded down. */
  [[nodiscard]] kb::Result<int32_t> ReadMillivolts() noexcept {
    constexpr uint32_t kRefMv = 3300U;
    const kb::Status s = take();
    const auto mv = static_cast<int32_t>((raw_ * kRefMv) / kFullScale);
    return (s == kb::Status::Ok) ? kb::Result<int32_t>::Success(mv)
                                 : kb::Result<int32_t>::Failure(s);
  }
  /** @brief AdcChannel::FullScale. */
  [[nodiscard]] uint32_t FullScale() const noexcept {
    return full_scale_;
  }

private:
  static constexpr uint32_t kFullScale = 4095U;
  [[nodiscard]] kb::Status take() noexcept {
    const kb::Status s = fail_;
    fail_ = kb::Status::Ok;
    return s;
  }
  uint32_t raw_{0U};
  uint32_t full_scale_{kFullScale};
  kb::Status fail_{kb::Status::Ok};
};

/** @brief A PWM channel that records what it was told. */
class FakePwm {
public:
  /** @brief PwmOutput::SetFrequencyHz; 0 or above 40 MHz is InvalidArgument. */
  [[nodiscard]] kb::Status SetFrequencyHz(uint32_t hz) noexcept {
    constexpr uint32_t kMaxHz = 40000000U;
    const bool ok = (hz != 0U) && (hz <= kMaxHz);
    if (ok) {
      hz_ = hz;
    }
    return ok ? kb::Status::Ok : kb::Status::InvalidArgument;
  }
  /** @brief PwmOutput::SetDuty. */
  [[nodiscard]] kb::Status SetDuty(Duty duty) noexcept {
    duty_ = duty;
    return kb::Status::Ok;
  }
  /** @brief PwmOutput::Enable. */
  [[nodiscard]] kb::Status Enable(bool on) noexcept {
    enabled_ = on;
    return kb::Status::Ok;
  }
  /** @brief The frequency last set. */
  [[nodiscard]] uint32_t FrequencyHz() const noexcept {
    return hz_;
  }
  /** @brief The duty last set. */
  [[nodiscard]] Duty DutyNow() const noexcept {
    return duty_;
  }
  /** @brief Whether the output runs. */
  [[nodiscard]] bool Enabled() const noexcept {
    return enabled_;
  }

private:
  uint32_t hz_{0U};
  Duty duty_;
  bool enabled_{false};
};

/** @brief A CAN controller with a transmit log and a receive queue. */
class FakeCanBus {
public:
  /** @brief Queues a frame for Receive; false when 32 are waiting. */
  [[nodiscard]] bool Inject(const CanFrame& frame) noexcept {
    return rx_.Push(frame);
  }
  /** @brief Frames sent so far (the first 32). */
  [[nodiscard]] std::span<const CanFrame> Sent() const noexcept {
    return tx_.AsSpan();
  }
  /** @brief While true every Send fails with BusError (bus-off). */
  void SetBusOff(bool off) noexcept {
    bus_off_ = off;
  }
  /** @brief CanBus::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ready_ = true;
    return kb::Status::Ok;
  }
  /** @brief CanBus::Send. */
  [[nodiscard]] kb::Status Send(const CanFrame& frame, kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout);
    kb::Status s = kb::Status::Ok;
    if (!ready_) {
      s = kb::Status::NotReady;
    } else if (frame.dlc > kCanMaxDlc) {
      s = kb::Status::InvalidArgument;
    } else if (bus_off_) {
      s = kb::Status::BusError;
    } else if (!tx_.PushBack(frame)) {
      s = kb::Status::Full;
    } else {
      s = kb::Status::Ok;
    }
    return s;
  }
  /** @brief CanBus::Receive: Timeout at once when nothing is queued. */
  [[nodiscard]] kb::Status Receive(CanFrame& frame, kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout);
    kb::Status s = kb::Status::Timeout;
    if (!ready_) {
      s = kb::Status::NotReady;
    } else if (rx_.Pop(frame)) {
      s = kb::Status::Ok;
    } else {
      s = kb::Status::Timeout;
    }
    return s;
  }

private:
  kb::utils::RingBuffer<CanFrame, 32> rx_;
  kb::utils::StaticVector<CanFrame, 32> tx_;
  bool ready_{false};
  bool bus_off_{false};
};

/** @brief A settable 64-bit monotonic clock. */
class FakeClock {
public:
  /** @brief Sets the reading. */
  void Set(kb::Instant now) noexcept {
    now_ = now;
  }
  /** @brief Moves the clock forward by `us`. */
  void Advance(uint64_t us) noexcept {
    now_ = kb::Instant{now_.Us() + us};
  }
  /** @brief MonotonicClock::Now. */
  [[nodiscard]] kb::Instant Now() const noexcept {
    return now_;
  }

private:
  kb::Instant now_;
};

/** @brief A busy wait that only adds up; advances an attached FakeClock. */
class FakeBusyWait {
public:
  /** @brief Advance `clock` by every wait (null detaches). */
  void Attach(FakeClock* clock) noexcept {
    clock_ = clock;
  }
  /** @brief BusyWait::WaitUs. */
  void WaitUs(kb::Microseconds us) noexcept {
    total_us_ += us.Count();
    if (clock_ != nullptr) {
      clock_->Advance(us.Count());
    }
  }
  /** @brief Microseconds waited in total. */
  [[nodiscard]] uint64_t TotalUs() const noexcept {
    return total_us_;
  }

private:
  FakeClock* clock_{nullptr};
  uint64_t total_us_{0U};
};

/** @brief A log sink that counts lines per level and keeps the last one. */
class FakeLogSink {
public:
  /** @brief LogSink::Write. */
  void Write(LogLevel level, std::string_view text) noexcept {
    const auto index = static_cast<std::size_t>(level);
    if (index < counts_.size()) {
      ++counts_[index];
    }
    last_.Clear();
    static_cast<void>(last_.Append(text));
  }
  /** @brief Lines written at `level`. */
  [[nodiscard]] uint32_t Count(LogLevel level) const noexcept {
    const auto index = static_cast<std::size_t>(level);
    return (index < counts_.size()) ? counts_[index] : 0U;
  }
  /** @brief The last line (cut at 128 characters). */
  [[nodiscard]] std::string_view Last() const noexcept {
    return last_.View();
  }

private:
  std::array<uint32_t, 4> counts_{};
  kb::utils::FixedString<128> last_;
};

static_assert(kb::iface::GpioOutput<FakeGpioPin> && kb::iface::GpioInterrupt<FakeGpioPin>,
              "FakeGpioPin conforms");
static_assert(kb::iface::AdcChannel<FakeAdc>, "FakeAdc conforms");
static_assert(kb::iface::PwmOutput<FakePwm>, "FakePwm conforms");
static_assert(kb::iface::CanBus<FakeCanBus>, "FakeCanBus conforms");
static_assert(kb::iface::MonotonicClock<FakeClock>, "FakeClock conforms");
static_assert(kb::iface::BusyWait<FakeBusyWait>, "FakeBusyWait conforms");
static_assert(kb::iface::LogSink<FakeLogSink>, "FakeLogSink conforms");

} // namespace kb::iface::host

#endif // KB_INTERFACES_HOST_FAKE_PINS_HPP
