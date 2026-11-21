/**
 * @file time.hpp
 * @brief Sleeps that wait at least the time asked, drift-free periodic waits, the tick and the
 * monotonic clock.
 *
 * - `SleepFor(Milliseconds)` and `SleepFor(Microseconds)` block for at least the duration: it is
 * converted to ticks rounded up, plus one tick for the partial tick in progress
 * (docs/rtos-boundary.md).
 * - `Periodic` wakes on a fixed grid (`Wait(period)`): no drift, and an overrun (the next wake
 * already passed) is reported and counted instead of hidden.
 * - `Now()` is the 64-bit monotonic clock in microseconds; `TickRateHz()` and `ToTicksAtLeast()`
 * expose the tick.
 *
 * Not callable from an interrupt handler.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_TIME_HPP
#define KB_RTOS_TIME_HPP

#include <cstdint>
#include <limits>

#include "kb/rtos/backend.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief Scheduler ticks per second. */
[[nodiscard]] inline uint32_t TickRateHz() noexcept {
  return backend::TickRateHz();
}

/** @brief The 64-bit monotonic clock in microseconds. */
[[nodiscard]] inline kb::Instant Now() noexcept {
  return backend::Now();
}

/**
 * @brief Ticks that cover at least `duration`: rounded up, plus one tick when `duration` is not
 * zero. `kb::kMaxMilliseconds` (and anything that saturates) gives UINT32_MAX, which backends treat
 * as "no limit".
 */
[[nodiscard]] inline uint32_t ToTicksAtLeast(Milliseconds duration) noexcept {
  constexpr uint32_t kForever = std::numeric_limits<uint32_t>::max();
  const uint32_t ticks = kb::utils::MillisecondsToTicksCeil(duration, backend::TickRateHz());
  uint32_t result = 0U;
  if (duration.Count() == 0U) {
    result = 0U;
  } else if (ticks >= (kForever - 1U)) {
    result = kForever;
  } else {
    result = ticks + 1U;
  }
  return result;
}

/** @brief Blocks the calling thread for at least `duration`; zero yields. */
inline void SleepFor(Milliseconds duration) noexcept {
  backend::SleepTicks(ToTicksAtLeast(duration));
}

/** @brief Blocks the calling thread for at least `duration`, rounded up to whole milliseconds. */
inline void SleepFor(Microseconds duration) noexcept {
  SleepFor(kb::utils::ToMillisecondsCeil(duration));
}

/** @brief Lets another ready thread of the same priority run. */
inline void Yield() noexcept {
  backend::Yield();
}

/**
 * @brief A fixed-rate wake-up grid for a periodic loop: `Wait(period)` returns at start + k *
 * period.
 *
 * The grid starts at the first `Wait()` (or `Reset()`). When a wake-up tick has already passed,
 * `Wait()` does not block, returns false and counts an overrun; the grid is kept, so a late step
 * does not shift later ones.
 */
class Periodic {
public:
  constexpr Periodic() noexcept = default;

  /** @brief Starts the grid now. */
  void Reset() noexcept {
    last_wake_ = backend::TickCount();
    started_ = true;
  }

  /**
   * @brief Waits for the next point of the grid, `period` (rounded up to whole ticks, at least one)
   * after the last.
   * @return False on an overrun.
   */
  [[nodiscard]] bool Wait(Milliseconds period) noexcept {
    if (!started_) {
      Reset();
    }
    const uint32_t raw = kb::utils::MillisecondsToTicksCeil(period, backend::TickRateHz());
    const uint32_t ticks = (raw == 0U) ? 1U : raw;
    const bool on_time = backend::SleepUntilTick(last_wake_, ticks);
    if (!on_time) {
      ++overruns_;
    }
    return on_time;
  }

  /** @brief Overruns counted since construction. */
  [[nodiscard]] uint32_t Overruns() const noexcept {
    return overruns_;
  }

private:
  uint32_t last_wake_{0U};
  uint32_t overruns_{0U};
  bool started_{false};
};

} // namespace kb::rtos

#endif // KB_RTOS_TIME_HPP
