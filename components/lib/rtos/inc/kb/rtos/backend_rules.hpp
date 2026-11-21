/**
 * @file backend_rules.hpp
 * @brief The platform-free rules a backend applies where its kernel lacks them: the delay-until
 * arithmetic, the non-recursive retake of a mutex that nests in the kernel, and the 64-bit
 * extension of a 32-bit tick counter. Pure functions and one value type, tested on the host; the
 * boundary files only call them (design: boundary files hold no logic, docs/rtos-boundary.md).
 *
 * Application code never uses this header.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_BACKEND_RULES_HPP
#define KB_RTOS_BACKEND_RULES_HPP

#include <cstdint>
#include <limits>

namespace kb::rtos::detail {

/** @brief What a delay-until does now: sleep `sleep_ticks` (none when zero) or report an overrun.
 */
struct DelayUntilPlan {
  uint32_t sleep_ticks{0U}; ///< Ticks to sleep; zero when the wake-up tick has come already.
  bool on_time{false};      ///< False: the wake-up tick had passed (or is now), no sleep.
};

/**
 * @brief The plan for waking at `previous_wake + period` when the tick is `now` (all modulo 2^32).
 * A step of exactly one period is an overrun: the wake-up tick has come.
 */
[[nodiscard]] constexpr DelayUntilPlan PlanDelayUntil(uint32_t now, uint32_t previous_wake,
                                                      uint32_t period) noexcept {
  const uint32_t elapsed = now - previous_wake;
  DelayUntilPlan plan{};
  if (elapsed < period) {
    plan.sleep_ticks = period - elapsed;
    plan.on_time = true;
  }
  return plan;
}

/**
 * @brief After the planned sleep, at tick `woke`: still on time unless the thread woke after the
 * grid point (a preemption between reading the tick and sleeping, or a late wake-up).
 */
[[nodiscard]] constexpr bool WokeOnTime(uint32_t woke, uint32_t previous_wake,
                                        uint32_t period) noexcept {
  return (woke - previous_wake) <= period;
}

/**
 * @brief Microseconds a monotonic 64-bit clock advanced from reading `last` to reading `now`
 * (modulo 2^64, so a counter wrap counts as forward). A reading that is behind `last` by less than
 * half the range is a clock that stepped back: it advanced by nothing.
 */
[[nodiscard]] constexpr uint64_t ForwardUs(uint64_t now, uint64_t last) noexcept {
  constexpr uint64_t kHalfRange = uint64_t{1U} << 63U;
  const uint64_t step = now - last;
  return (step < kHalfRange) ? step : 0U;
}

/**
 * @brief Tests `done` until it holds or `limit_us` of clock time has passed, sleeping with `nap`
 * (one tick) between tests. The time is the sum of the clock's forward steps, so a clock that steps
 * back never shortens the wait (it fails closed: the wait takes at least `limit_us` of forward
 * time) and a counter wrap does not end it early. True when `done` held.
 * @param clock Returns the monotonic clock in microseconds.
 */
template <typename Clock, typename Nap, typename Done>
[[nodiscard]] bool PollUntil(Clock clock, Nap nap, Done done, uint64_t limit_us) noexcept {
  uint64_t last = clock();
  uint64_t spent = 0U;
  bool held = done();
  while ((!held) && (spent < limit_us)) {
    nap();
    const uint64_t now = clock();
    spent += ForwardUs(now, last);
    last = now;
    held = done();
  }
  return held;
}

/**
 * @brief The sleep before failing when the holder of a non-recursive kb mutex asks for it again on
 * a kernel whose mutexes nest (ThreadX): the whole timeout, as a real wait would take, but none
 * for an unbounded wait, which could never end (it fails at once).
 */
[[nodiscard]] constexpr uint32_t HolderRetakeSleep(uint32_t timeout_ticks) noexcept {
  return (timeout_ticks == std::numeric_limits<uint32_t>::max()) ? 0U : timeout_ticks;
}

/**
 * @brief A 32-bit tick counter extended to 64 bits: each reading lower than the last counts a
 * wrap. Readings must come at least once per wrap period; the owner serialises the calls.
 */
class TickExtender {
public:
  /** @brief The 64-bit tick of the reading `tick`. */
  [[nodiscard]] constexpr uint64_t Extend(uint32_t tick) noexcept {
    if (tick < last_tick_) {
      ++wraps_;
    }
    last_tick_ = tick;
    constexpr uint32_t kHighShift = 32U;
    return (static_cast<uint64_t>(wraps_) << kHighShift) | static_cast<uint64_t>(tick);
  }

private:
  uint32_t last_tick_{0U};
  uint32_t wraps_{0U};
};

} // namespace kb::rtos::detail

#endif // KB_RTOS_BACKEND_RULES_HPP
