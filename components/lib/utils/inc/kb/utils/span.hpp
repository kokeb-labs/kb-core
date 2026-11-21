/**
 * @file span.hpp
 * @brief Bounded helpers over std::span: checked sub-spans and copies, fill, equality and a
 *        constant-time comparison for secrets.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_SPAN_HPP
#define KB_UTILS_SPAN_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace kb::utils {

/**
 * @brief `count` elements of `whole` from `offset` into `out`, when they lie inside `whole`.
 * @return False (and `out` unchanged) when `offset + count` passes the end; no arithmetic can wrap.
 */
template <typename T>
[[nodiscard]] constexpr bool CheckedSubspan(std::span<T> whole, std::size_t offset,
                                            std::size_t count, std::span<T>& out) noexcept {
  const bool inside = (offset <= whole.size()) && (count <= (whole.size() - offset));
  if (inside) {
    out = whole.subspan(offset, count);
  }
  return inside;
}

/**
 * @brief Copies `source` to the start of `destination` when it fits.
 * @return False (and nothing copied) when `source` is longer than `destination`.
 */
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] constexpr bool CopyInto(std::span<T> destination,
                                      std::span<const T> source) noexcept {
  const bool fits = source.size() <= destination.size();
  if (fits) {
    for (std::size_t i = 0U; i < source.size(); ++i) {
      destination[i] = source[i];
    }
  }
  return fits;
}

/** @brief Sets every element of `destination` to `value`. */
template <typename T>
  requires std::is_trivially_copyable_v<T>
constexpr void Fill(std::span<T> destination, const T& value) noexcept {
  for (T& element : destination) {
    element = value;
  }
}

/** @brief True when both spans have the same length and elements. */
template <typename T>
[[nodiscard]] constexpr bool Equal(std::span<const T> a, std::span<const T> b) noexcept {
  bool same = a.size() == b.size();
  for (std::size_t i = 0U; same && (i < a.size()); ++i) {
    same = a[i] == b[i];
  }
  return same;
}

/**
 * @brief Byte equality in time that depends only on the length, not on where the first difference
 * is (for MACs, keys and authentication tags). Different lengths compare unequal at once.
 */
[[nodiscard]] constexpr bool ConstantTimeEqual(std::span<const uint8_t> a,
                                               std::span<const uint8_t> b) noexcept {
  uint32_t difference = (a.size() == b.size()) ? 0U : 1U;
  if (difference == 0U) {
    for (std::size_t i = 0U; i < a.size(); ++i) {
      difference |= static_cast<uint32_t>(a[i]) ^ static_cast<uint32_t>(b[i]);
    }
  }
  return difference == 0U;
}

/** @brief The bytes of a trivially copyable object, for CRCs and frame building. */
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] std::span<const std::byte, sizeof(T)> ObjectBytes(const T& object) noexcept {
  return std::as_bytes(std::span<const T, 1>{&object, 1U});
}

} // namespace kb::utils

#endif // KB_UTILS_SPAN_HPP
