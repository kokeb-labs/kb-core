/**
 * @file fixed_string.hpp
 * @brief kb::utils::FixedString<N>: a NUL-terminated text buffer of at most N characters, and
 * number formatting into it, with no heap and no printf.
 *
 * Appends that do not fit are cut at the capacity, set `Truncated()` and return false; the text
 * stays terminated. Numbers are formatted by the class itself, so no `PRId64` or newlib format
 * support is needed.
 *
 * @code
 * kb::utils::FixedString<32> line{};
 * static_cast<void>(line.Append("tick="));
 * static_cast<void>(line.AppendUnsigned(1234U));
 * static_cast<void>(line.Append(" status=0x"));
 * static_cast<void>(line.AppendHex(0x2AU, 2U));
 * log.Write(line.CStr());   // "tick=1234 status=0x2A"
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_FIXED_STRING_HPP
#define KB_UTILS_FIXED_STRING_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kb::utils {

namespace detail {
/// Digits for every base up to 16, upper case: indexed by a digit value, never computed from a
/// char.
inline constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
/// The most digits a 64-bit value needs (base 2).
inline constexpr std::size_t kMaxDigits = 64U;
} // namespace detail

/**
 * @brief Text of at most N characters plus a terminating NUL, stored inline.
 * @tparam N Capacity in characters, at least 1.
 */
template <std::size_t N>
  requires(N > 0U)
class FixedString {
public:
  constexpr FixedString() noexcept = default;

  /** @brief Appends `text`; false and Truncated() when it did not fit whole. */
  [[nodiscard]] constexpr bool Append(std::string_view text) noexcept {
    bool whole = true;
    for (const char c : text) {
      if (length_ < N) {
        chars_[length_] = c;
        ++length_;
      } else {
        whole = false;
      }
    }
    chars_[length_] = '\0';
    if (!whole) {
      truncated_ = true;
    }
    return whole;
  }

  /** @brief Appends one character. */
  [[nodiscard]] constexpr bool AppendChar(char c) noexcept {
    const std::array<char, 1> one{c};
    return Append(std::string_view{one.data(), one.size()});
  }

  /**
   * @brief Appends `value` in `base` (2 to 16, upper-case digits), at least `min_digits` digits
   * with leading zeros. A base outside 2..16 appends nothing and returns false.
   */
  [[nodiscard]] constexpr bool AppendUnsigned(uint64_t value, uint32_t base = 10U,
                                              std::size_t min_digits = 1U) noexcept {
    bool ok = (base >= 2U) && (base <= 16U);
    if (ok) {
      std::array<char, detail::kMaxDigits> digits{};
      std::size_t count = 0U;
      uint64_t rest = value;
      const std::size_t wanted =
          (min_digits < detail::kMaxDigits) ? min_digits : detail::kMaxDigits;
      do {
        digits[count] = detail::kDigits[static_cast<std::size_t>(rest % base)];
        rest /= base;
        ++count;
      } while ((rest != 0U) || (count < wanted));
      // The digits are least significant first: append them in reverse.
      std::array<char, detail::kMaxDigits> ordered{};
      for (std::size_t i = 0U; i < count; ++i) {
        ordered[i] = digits[count - 1U - i];
      }
      ok = Append(std::string_view{ordered.data(), count});
    }
    return ok;
  }

  /** @brief Appends `value` in decimal with a leading '-' when negative. */
  [[nodiscard]] constexpr bool AppendSigned(int64_t value) noexcept {
    bool ok = true;
    auto magnitude = static_cast<uint64_t>(value);
    if (value < 0) {
      ok = AppendChar('-');
      // Two's complement negation in unsigned arithmetic: defined for INT64_MIN as well.
      magnitude = 0U - magnitude;
    }
    const bool digits_ok = AppendUnsigned(magnitude);
    return ok && digits_ok;
  }

  /** @brief Appends `value` in upper-case hexadecimal, at least `min_digits` digits, no prefix. */
  [[nodiscard]] constexpr bool AppendHex(uint64_t value, std::size_t min_digits = 1U) noexcept {
    return AppendUnsigned(value, 16U, min_digits);
  }

  /** @brief Appends "true" or "false". */
  [[nodiscard]] constexpr bool AppendBool(bool value) noexcept {
    return Append(value ? std::string_view{"true"} : std::string_view{"false"});
  }

  /** @brief Empties the text and clears Truncated(). */
  constexpr void Clear() noexcept {
    length_ = 0U;
    chars_[0] = '\0';
    truncated_ = false;
  }

  /** @brief The text. */
  [[nodiscard]] constexpr std::string_view View() const noexcept {
    return std::string_view{chars_.data(), length_};
  }
  /** @brief The text, NUL-terminated; valid while the string lives and is not changed. */
  [[nodiscard]] constexpr const char* CStr() const noexcept {
    return chars_.data();
  }
  /** @brief Characters held. */
  [[nodiscard]] constexpr std::size_t Length() const noexcept {
    return length_;
  }
  /** @brief The capacity N. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return N;
  }
  /** @brief True when an append since the last Clear() was cut. */
  [[nodiscard]] constexpr bool Truncated() const noexcept {
    return truncated_;
  }

private:
  static constexpr std::size_t kStorage = N + 1U;
  std::array<char, kStorage> chars_{};
  std::size_t length_{0U};
  bool truncated_{false};
};

} // namespace kb::utils

#endif // KB_UTILS_FIXED_STRING_HPP
