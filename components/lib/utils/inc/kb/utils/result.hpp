/**
 * @file result.hpp
 * @brief kb::Result<T>: a value or a kb::Status, with a vendor detail code. No exceptions, no heap.
 *
 * A result is made by its two factories and read by its accessors; it has no public data. The
 * factories build a value-initialised object and then set its fields (`Result r{}; r.error_ = e;
 * return r;`), the form the MISRA C++:2023 scan accepts for Rule 0.2.2 where an aggregate
 * initialiser is not.
 *
 * @code
 * kb::Result<uint16_t> ReadId() noexcept {
 *   if (!ready) { return kb::Result<uint16_t>::Failure(kb::Status::NotReady); }
 *   return kb::Result<uint16_t>::Success(0x5A5AU);
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_RESULT_HPP
#define KB_UTILS_RESULT_HPP

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "kb/utils/status.hpp"

namespace kb {

/** @brief What a Result<T> may carry: a copyable value type with a default (no references, no
 * arrays). */
template <typename T>
concept ResultValue = std::is_object_v<T> && (!std::is_array_v<T>) && std::semiregular<T> &&
                      std::is_nothrow_copy_constructible_v<T>;

/**
 * @brief A value of type T, or the reason there is none.
 *
 * `Failure(Status::Ok)` is a programming error; it is recorded as `Status::Fault`, so a failure can
 * never read as a success.
 *
 * @tparam T The value type (ResultValue); `void` selects the specialisation below.
 */
template <typename T>
  requires(ResultValue<T> || std::is_void_v<T>)
class [[nodiscard]] Result {
public:
  /** @brief An empty failure result (`Status::NotReady`), so a default object never reads as
   * success. */
  constexpr Result() noexcept = default;

  /** @brief A success holding `value`. */
  [[nodiscard]] static constexpr Result Success(const T& value) noexcept {
    Result r{};
    r.value_ = value;
    r.error_ = Status::Ok;
    return r;
  }

  /** @brief A failure with `error` (never Ok) and an optional vendor `detail` code. */
  [[nodiscard]] static constexpr Result Failure(Status error, uint32_t detail = 0U) noexcept {
    Result r{};
    r.error_ = (error == Status::Ok) ? Status::Fault : error;
    r.detail_ = detail;
    return r;
  }

  /** @brief True when the result holds a value. */
  [[nodiscard]] constexpr bool IsOk() const noexcept {
    return error_ == Status::Ok;
  }

  /** @brief `Status::Ok` on success, otherwise the failure. */
  [[nodiscard]] constexpr Status Error() const noexcept {
    return error_;
  }

  /** @brief The vendor code a port recorded with the failure; 0 when none. */
  [[nodiscard]] constexpr uint32_t Detail() const noexcept {
    return detail_;
  }

  /** @brief The value. Precondition: IsOk(); on a failure it is the value-initialised T. */
  [[nodiscard]] constexpr const T& Value() const noexcept {
    return value_;
  }

  /** @brief The value on success, `fallback` on failure. */
  [[nodiscard]] constexpr T ValueOr(const T& fallback) const noexcept {
    return IsOk() ? value_ : fallback;
  }

private:
  T value_{};
  Status error_{Status::NotReady};
  uint32_t detail_{0U};
};

/** @brief Success or the reason for failure, with no value. */
template <>
class [[nodiscard]] Result<void> {
public:
  /** @brief An empty failure result (`Status::NotReady`). */
  constexpr Result() noexcept = default;

  /** @brief A success. */
  [[nodiscard]] static constexpr Result Success() noexcept {
    Result r{};
    r.error_ = Status::Ok;
    return r;
  }

  /** @brief A failure with `error` (never Ok) and an optional vendor `detail` code. */
  [[nodiscard]] static constexpr Result Failure(Status error, uint32_t detail = 0U) noexcept {
    Result r{};
    r.error_ = (error == Status::Ok) ? Status::Fault : error;
    r.detail_ = detail;
    return r;
  }

  /** @brief A result from a bare status: Success() for Ok, else Failure(status). */
  [[nodiscard]] static constexpr Result FromStatus(Status status) noexcept {
    return (status == Status::Ok) ? Success() : Failure(status);
  }

  /** @brief True on success. */
  [[nodiscard]] constexpr bool IsOk() const noexcept {
    return error_ == Status::Ok;
  }

  /** @brief `Status::Ok` on success, otherwise the failure. */
  [[nodiscard]] constexpr Status Error() const noexcept {
    return error_;
  }

  /** @brief The vendor code a port recorded with the failure; 0 when none. */
  [[nodiscard]] constexpr uint32_t Detail() const noexcept {
    return detail_;
  }

private:
  Status error_{Status::NotReady};
  uint32_t detail_{0U};
};

/** @brief The status of any result, for code that only forwards the outcome. */
template <typename T>
[[nodiscard]] constexpr Status ToStatus(const Result<T>& result) noexcept {
  return result.Error();
}

} // namespace kb

#endif // KB_UTILS_RESULT_HPP
