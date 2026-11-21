/**
 * @file time.hpp
 * @brief Typed durations, the 64-bit monotonic instant, and wrap-safe elapsed-time arithmetic.
 *
 * - `kb::Microseconds`, `kb::Milliseconds`: durations as distinct types with a 32-bit count, so a
 *   millisecond value can never be passed where microseconds are expected. Conversions between them
 * are explicit and round in the safe direction (`ToMicroseconds` saturates, `ToMillisecondsCeil`
 * rounds up).
 * - `kb::Instant`: a reading of the one 64-bit monotonic microsecond clock above the ports.
 * - Elapsed time is always an unsigned subtraction (`now - since`), correct across a wrap of the
 * counter. Timestamps are never compared with each other (no half-range compare): a deadline is
 * "elapsed >= timeout".
 *
 * There are no user-defined literals: write `kb::Milliseconds{10U}`.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_TIME_HPP
#define KB_UTILS_TIME_HPP

#include <compare>
#include <cstdint>
#include <limits>

namespace kb {

/** @brief A duration in microseconds (32-bit count: up to about 71.6 minutes). */
class Microseconds {
public:
  constexpr Microseconds() noexcept = default;
  /** @brief `count` microseconds. */
  constexpr explicit Microseconds(uint32_t count) noexcept : count_(count) {}
  /** @brief The count of microseconds. */
  [[nodiscard]] constexpr uint32_t Count() const noexcept {
    return count_;
  }

private:
  uint32_t count_{0U};
};

/** @brief A duration in milliseconds (32-bit count: up to about 49.7 days). */
class Milliseconds {
public:
  constexpr Milliseconds() noexcept = default;
  /** @brief `count` milliseconds. */
  constexpr explicit Milliseconds(uint32_t count) noexcept : count_(count) {}
  /** @brief The count of milliseconds. */
  [[nodiscard]] constexpr uint32_t Count() const noexcept {
    return count_;
  }

private:
  uint32_t count_{0U};
};

/** @brief A reading of the 64-bit monotonic microsecond clock (microseconds since an arbitrary
 * start). */
class Instant {
public:
  constexpr Instant() noexcept = default;
  /** @brief The instant `us` microseconds after the clock's start. */
  constexpr explicit Instant(uint64_t us) noexcept : us_(us) {}
  /** @brief Microseconds since the clock's start. */
  [[nodiscard]] constexpr uint64_t Us() const noexcept {
    return us_;
  }

private:
  uint64_t us_{0U};
};

/** @brief Equality of two durations. */
[[nodiscard]] constexpr bool operator==(Microseconds a, Microseconds b) noexcept {
  return a.Count() == b.Count();
}
/** @brief Ordering of two durations. */
[[nodiscard]] constexpr std::strong_ordering operator<=>(Microseconds a, Microseconds b) noexcept {
  return a.Count() <=> b.Count();
}
/** @brief Equality of two durations. */
[[nodiscard]] constexpr bool operator==(Milliseconds a, Milliseconds b) noexcept {
  return a.Count() == b.Count();
}
/** @brief Ordering of two durations. */
[[nodiscard]] constexpr std::strong_ordering operator<=>(Milliseconds a, Milliseconds b) noexcept {
  return a.Count() <=> b.Count();
}
/** @brief Equality of two instants (the same reading); instants are never ordered (use elapsed
 * time). */
[[nodiscard]] constexpr bool operator==(Instant a, Instant b) noexcept {
  return a.Us() == b.Us();
}

/** @brief The longest representable wait: callers use it for "wait without a practical bound". */
inline constexpr Milliseconds kMaxMilliseconds{std::numeric_limits<uint32_t>::max()};

} // namespace kb

namespace kb::utils {

/** @brief `ms` as microseconds, saturating at the largest Microseconds. */
[[nodiscard]] constexpr Microseconds ToMicroseconds(Milliseconds ms) noexcept {
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  constexpr uint32_t kPerMs = 1000U;
  const uint32_t count = (ms.Count() > (kMax / kPerMs)) ? kMax : (ms.Count() * kPerMs);
  return Microseconds{count};
}

/** @brief `us` as milliseconds, rounded up (a wait converted this way is never shorter). */
[[nodiscard]] constexpr Milliseconds ToMillisecondsCeil(Microseconds us) noexcept {
  constexpr uint32_t kPerMs = 1000U;
  const uint32_t whole = us.Count() / kPerMs;
  const uint32_t count = ((us.Count() % kPerMs) != 0U) ? (whole + 1U) : whole;
  return Milliseconds{count};
}

/**
 * @brief Scheduler ticks that cover at least `ms` at `tick_hz`: `ceil(ms * tick_hz / 1000)`.
 *
 * Saturates at UINT32_MAX. A `tick_hz` of 0 has no tick to count and also gives UINT32_MAX, so a
 * bad configuration waits too long rather than not at all. A backend adds one more tick for the
 * partial tick in progress (rtos-boundary rule "at least").
 */
[[nodiscard]] constexpr uint32_t MillisecondsToTicksCeil(Milliseconds ms,
                                                         uint32_t tick_hz) noexcept {
  constexpr uint64_t kPerS = 1000U;
  constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
  uint64_t ticks = kMax;
  if (tick_hz != 0U) {
    const uint64_t product = static_cast<uint64_t>(ms.Count()) * static_cast<uint64_t>(tick_hz);
    const uint64_t whole = product / kPerS;
    ticks = ((product % kPerS) != 0U) ? (whole + 1U) : whole;
  }
  return (ticks > kMax) ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(ticks);
}

/** @brief Microseconds from `since` to `now` on the 64-bit clock, by unsigned subtraction. */
[[nodiscard]] constexpr uint64_t ElapsedUs(Instant now, Instant since) noexcept {
  return now.Us() - since.Us();
}

/** @brief Ticks from `since` to `now` on a wrapping 32-bit counter, correct across one wrap. */
[[nodiscard]] constexpr uint32_t ElapsedSince(uint32_t now, uint32_t since) noexcept {
  return now - since;
}

/** @brief True when at least `timeout` ticks passed from `since` to `now` on a wrapping 32-bit
 * counter. */
[[nodiscard]] constexpr bool HasElapsed(uint32_t now, uint32_t since, uint32_t timeout) noexcept {
  return ElapsedSince(now, since) >= timeout;
}

/** @brief The low 32 bits of an instant, for driver hooks with a 32-bit microsecond clock. */
[[nodiscard]] constexpr uint32_t NarrowUs(Instant now) noexcept {
  constexpr uint64_t kLow = std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(now.Us() & kLow);
}

/** @brief Whole milliseconds of an instant, low 32 bits, for driver hooks with a 32-bit ms clock.
 */
[[nodiscard]] constexpr uint32_t NarrowMs(Instant now) noexcept {
  constexpr uint64_t kLow = std::numeric_limits<uint32_t>::max();
  constexpr uint64_t kPerMs = 1000U;
  return static_cast<uint32_t>((now.Us() / kPerMs) & kLow);
}

/**
 * @brief A time limit started at an instant: expired once `timeout` has elapsed on the 64-bit
 * clock.
 *
 * Built by `Deadline::After(now, timeout)`; checked by `Expired(now)` and `RemainingUs(now)`.
 */
class Deadline {
public:
  constexpr Deadline() noexcept = default;

  /** @brief A deadline `timeout` after `start`. */
  [[nodiscard]] static constexpr Deadline After(Instant start, Microseconds timeout) noexcept {
    Deadline d{};
    d.start_ = start;
    d.timeout_us_ = timeout.Count();
    return d;
  }

  /** @brief A deadline `timeout` after `start`. */
  [[nodiscard]] static constexpr Deadline After(Instant start, Milliseconds timeout) noexcept {
    constexpr uint64_t kPerMs = 1000U;
    Deadline d{};
    d.start_ = start;
    d.timeout_us_ = static_cast<uint64_t>(timeout.Count()) * kPerMs;
    return d;
  }

  /** @brief True when the timeout has elapsed at `now`. */
  [[nodiscard]] constexpr bool Expired(Instant now) const noexcept {
    return ElapsedUs(now, start_) >= timeout_us_;
  }

  /** @brief Microseconds left at `now`; 0 once expired. */
  [[nodiscard]] constexpr uint64_t RemainingUs(Instant now) const noexcept {
    const uint64_t elapsed = ElapsedUs(now, start_);
    return (elapsed >= timeout_us_) ? 0U : (timeout_us_ - elapsed);
  }

private:
  Instant start_;
  uint64_t timeout_us_{0U};
};

} // namespace kb::utils

#endif // KB_UTILS_TIME_HPP
