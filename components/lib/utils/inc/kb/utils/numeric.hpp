/**
 * @file numeric.hpp
 * @brief Checked, saturating and range-checked integer arithmetic, free of undefined behaviour.
 *
 * - `CheckedAdd`, `CheckedSub`, `CheckedMul`: the exact result in `out` and true, or false and
 * `out` unchanged when the result does not fit T.
 * - `SaturatingAdd`, `SaturatingSub`, `SaturatingMul`: the exact result, or the nearest limit of T.
 * - `InRangeCast<To>(from, out)`: converts only when the value fits; `Saturate<To>(from)` clamps.
 *
 * Every function is constexpr and works for every standard integer type except `bool` and the
 * character types. Arithmetic is done in `std::intmax_t` or `std::uintmax_t` after a range check,
 * so no operand is promoted to `int` and no intermediate overflows.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_NUMERIC_HPP
#define KB_UTILS_NUMERIC_HPP

#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace kb::utils {

/** @brief An integer type for arithmetic: integral, not `bool`, not a character type. */
template <typename T>
concept Integer =
    std::integral<T> && (!std::same_as<std::remove_cv_t<T>, bool>) &&
    (!std::same_as<std::remove_cv_t<T>, char>) && (!std::same_as<std::remove_cv_t<T>, wchar_t>) &&
    (!std::same_as<std::remove_cv_t<T>, char8_t>) &&
    (!std::same_as<std::remove_cv_t<T>, char16_t>) &&
    (!std::same_as<std::remove_cv_t<T>, char32_t>);

/** @brief True when `value` is representable in `To`. */
template <Integer To, Integer From>
[[nodiscard]] constexpr bool InRange(From value) noexcept {
  return std::in_range<To>(value);
}

/** @brief Converts `value` to `To` into `out` when it fits; false and `out` unchanged otherwise. */
template <Integer To, Integer From>
[[nodiscard]] constexpr bool InRangeCast(From value, To& out) noexcept {
  bool fits = false;
  if (std::in_range<To>(value)) {
    out = static_cast<To>(value);
    fits = true;
  }
  return fits;
}

/** @brief `value` converted to `To`, clamped to `To`'s limits. */
template <Integer To, Integer From>
[[nodiscard]] constexpr To Saturate(From value) noexcept {
  To result = std::numeric_limits<To>::max();
  if (std::cmp_less(value, std::numeric_limits<To>::min())) {
    result = std::numeric_limits<To>::min();
  } else if (std::cmp_greater(value, std::numeric_limits<To>::max())) {
    result = std::numeric_limits<To>::max();
  } else {
    result = static_cast<To>(value);
  }
  return result;
}

/** @brief `value` limited to `[low, high]`. Precondition: `low <= high` (otherwise `low`). */
template <Integer T>
[[nodiscard]] constexpr T Clamp(T value, T low, T high) noexcept {
  T result = value;
  if (value < low) {
    result = low;
  } else if (value > high) {
    result = high;
  } else {
    result = value;
  }
  return (low <= high) ? result : low;
}

namespace detail {

/** @brief The widest integer of T's signedness: where the checked operations compute. */
template <Integer T>
using Wide = std::conditional_t<std::is_signed_v<T>, std::intmax_t, std::uintmax_t>;

/** @brief `a + b` in the wide type when it cannot overflow it; false when it would. */
template <Integer T>
[[nodiscard]] constexpr bool WideAdd(Wide<T> a, Wide<T> b, Wide<T>& out) noexcept {
  using W = Wide<T>;
  bool ok = true;
  if constexpr (std::is_signed_v<T>) {
    if ((b > 0) && (a > (std::numeric_limits<W>::max() - b))) {
      ok = false;
    } else if ((b < 0) && (a < (std::numeric_limits<W>::min() - b))) {
      ok = false;
    } else {
      out = a + b;
    }
  } else {
    if (a > (std::numeric_limits<W>::max() - b)) {
      ok = false;
    } else {
      out = a + b;
    }
  }
  return ok;
}

/** @brief `a - b` in the wide type when it cannot overflow it; false when it would. */
template <Integer T>
[[nodiscard]] constexpr bool WideSub(Wide<T> a, Wide<T> b, Wide<T>& out) noexcept {
  using W = Wide<T>;
  bool ok = true;
  if constexpr (std::is_signed_v<T>) {
    if ((b < 0) && (a > (std::numeric_limits<W>::max() + b))) {
      ok = false;
    } else if ((b > 0) && (a < (std::numeric_limits<W>::min() + b))) {
      ok = false;
    } else {
      out = a - b;
    }
  } else {
    if (a < b) {
      ok = false;
    } else {
      out = a - b;
    }
  }
  return ok;
}

/** @brief `a * b` in the wide type when it cannot overflow it; false when it would. */
template <Integer T>
[[nodiscard]] constexpr bool WideMul(Wide<T> a, Wide<T> b, Wide<T>& out) noexcept {
  using W = Wide<T>;
  constexpr W kMax = std::numeric_limits<W>::max();
  bool ok = true;
  if ((a == 0) || (b == 0)) {
    out = 0;
  } else if constexpr (std::is_signed_v<T>) {
    constexpr W kMin = std::numeric_limits<W>::min();
    if (a > 0) {
      ok = (b > 0) ? (a <= (kMax / b)) : (b >= (kMin / a));
    } else {
      ok = (b > 0) ? (a >= (kMin / b)) : (b >= (kMax / a));
    }
    if (ok) {
      out = a * b;
    }
  } else {
    ok = (a <= (kMax / b));
    if (ok) {
      out = a * b;
    }
  }
  return ok;
}

/** @brief Stores a wide result into `out` when it fits T. */
template <Integer T>
[[nodiscard]] constexpr bool Narrow(bool computed, Wide<T> wide, T& out) noexcept {
  return computed && InRangeCast<T>(wide, out);
}

} // namespace detail

/** @brief `a + b` into `out` when it fits T; false and `out` unchanged otherwise. */
template <Integer T>
[[nodiscard]] constexpr bool CheckedAdd(T a, T b, T& out) noexcept {
  detail::Wide<T> wide = 0;
  const bool computed = detail::WideAdd<T>(a, b, wide);
  return detail::Narrow<T>(computed, wide, out);
}

/** @brief `a - b` into `out` when it fits T; false and `out` unchanged otherwise. */
template <Integer T>
[[nodiscard]] constexpr bool CheckedSub(T a, T b, T& out) noexcept {
  detail::Wide<T> wide = 0;
  const bool computed = detail::WideSub<T>(a, b, wide);
  return detail::Narrow<T>(computed, wide, out);
}

/** @brief `a * b` into `out` when it fits T; false and `out` unchanged otherwise. */
template <Integer T>
[[nodiscard]] constexpr bool CheckedMul(T a, T b, T& out) noexcept {
  detail::Wide<T> wide = 0;
  const bool computed = detail::WideMul<T>(a, b, wide);
  return detail::Narrow<T>(computed, wide, out);
}

/** @brief `a + b`, or T's limit on the side the exact result lies. */
template <Integer T>
[[nodiscard]] constexpr T SaturatingAdd(T a, T b) noexcept {
  T out = 0;
  if (!CheckedAdd(a, b, out)) {
    out = (std::is_signed_v<T> && (b < 0)) ? std::numeric_limits<T>::min()
                                           : std::numeric_limits<T>::max();
  }
  return out;
}

/** @brief `a - b`, or T's limit on the side the exact result lies. */
template <Integer T>
[[nodiscard]] constexpr T SaturatingSub(T a, T b) noexcept {
  T out = 0;
  if (!CheckedSub(a, b, out)) {
    out = (std::is_signed_v<T> && (b < 0)) ? std::numeric_limits<T>::max()
                                           : std::numeric_limits<T>::min();
  }
  return out;
}

/** @brief `a * b`, or T's limit on the side the exact result lies. */
template <Integer T>
[[nodiscard]] constexpr T SaturatingMul(T a, T b) noexcept {
  T out = 0;
  if (!CheckedMul(a, b, out)) {
    const bool negative = std::is_signed_v<T> && ((a < 0) != (b < 0));
    out = negative ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
  }
  return out;
}

/** @brief `ceil(numerator / denominator)` for unsigned values; 0 when `denominator` is 0. */
template <Integer T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr T DivCeil(T numerator, T denominator) noexcept {
  T result = 0U;
  if (denominator != 0U) {
    const T quotient = static_cast<T>(numerator / denominator);
    const T remainder = static_cast<T>(numerator % denominator);
    result = (remainder != 0U) ? static_cast<T>(quotient + 1U) : quotient;
  }
  return result;
}

} // namespace kb::utils

#endif // KB_UTILS_NUMERIC_HPP
