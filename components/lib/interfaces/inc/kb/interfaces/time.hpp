/**
 * @file time.hpp
 * @brief The clock and delay contracts: `MonotonicClock` (the one 64-bit microsecond clock above
 * the ports) and `BusyWait` (short delays below one millisecond), with `MonotonicClockRef` and
 * `BusyWaitRef`.
 *
 * | Concept | Member | Meaning |
 * | --- | --- | --- |
 * | `MonotonicClock` | `kb::Instant Now() const noexcept` | Microseconds since an arbitrary start;
 * never goes back, never wraps in practice | | `BusyWait` | `void WaitUs(kb::Microseconds us)
 * noexcept` | Spins for at least `us`; for waits below 1 ms (longer ones use kb::rtos::SleepFor) |
 *
 * Driver hooks with a 32-bit clock narrow `Now()` with kb::utils::NarrowUs / NarrowMs and compare
 * by unsigned elapsed time (kb::utils::ElapsedSince).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_TIME_HPP
#define KB_INTERFACES_TIME_HPP

#include <concepts>

#include "kb/utils/time.hpp"

namespace kb::iface {

/** @brief The 64-bit monotonic microsecond clock. */
template <typename C>
concept MonotonicClock = requires(const C& clock) {
  { clock.Now() } noexcept -> std::same_as<kb::Instant>;
};

/** @brief A short spin delay. */
template <typename D>
concept BusyWait = requires(D& delay, kb::Microseconds us) {
  { delay.WaitUs(us) } noexcept -> std::same_as<void>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_TIME_HPP
