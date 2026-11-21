/**
 * @file driver_result.hpp
 * @brief Turning a driver's own `Result<T>` (`{value, error}` with `ok()`, the base set of every
 * kokeb-labs driver) into a `kb::Result<T>`: the binding's one `ToStatus()` map gives the
 * `kb::Status`, and the driver's error code travels in `Detail()` (the vendor detail of the health
 * record).
 *
 * @code
 * return kb::handlers::Lift(driver.ReadInputs(), &pcal9555_binding::ToStatus);  // Result<uint16_t>
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_DRIVER_RESULT_HPP
#define KB_HANDLERS_COMMON_DRIVER_RESULT_HPP

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"

namespace kb::handlers {

/** @brief A driver error enumeration (scoped, integral underlying type). */
template <typename E>
concept DriverError = std::is_enum_v<E> && std::is_integral_v<std::underlying_type_t<E>>;

/** @brief A driver's result without a value: an `error` field and `ok()`. */
template <typename R>
concept DriverStatusResult = requires(const R& r) {
  { r.ok() } noexcept -> std::same_as<bool>;
  requires DriverError<std::remove_cvref_t<decltype(r.error)>>;
};

/** @brief A driver's result with a value: also a `value` field. */
template <typename R>
concept DriverValueResult = DriverStatusResult<R> && requires(const R& r) {
  requires ResultValue<std::remove_cvref_t<decltype(r.value)>>;
};

/** @brief The driver error code as the vendor detail of a kb result. */
template <DriverError E>
[[nodiscard]] constexpr uint32_t DetailOf(E error) noexcept {
  return static_cast<uint32_t>(static_cast<std::underlying_type_t<E>>(error));
}

/**
 * @brief A driver result without a value as `kb::Result<void>`.
 * @param result The driver's result.
 * @param map The binding's map of every driver error to a kb::Status.
 */
template <DriverStatusResult R, typename Map>
[[nodiscard]] constexpr kb::Result<void> LiftStatus(const R& result, Map map) noexcept {
  return result.ok() ? kb::Result<void>::Success()
                     : kb::Result<void>::Failure(map(result.error), DetailOf(result.error));
}

/**
 * @brief A driver result with a value as `kb::Result<T>`.
 * @param result The driver's result.
 * @param map The binding's map of every driver error to a kb::Status.
 */
template <DriverValueResult R, typename Map>
[[nodiscard]] constexpr auto Lift(const R& result, Map map) noexcept
    -> kb::Result<std::remove_cvref_t<decltype(result.value)>> {
  return result.ok()
             ? kb::Result<std::remove_cvref_t<decltype(result.value)>>::Success(result.value)
             : kb::Result<std::remove_cvref_t<decltype(result.value)>>::Failure(
                   map(result.error), DetailOf(result.error));
}

/** @brief The failure of `from` carried into a result of another type (status and detail). */
template <typename To, typename From>
[[nodiscard]] constexpr kb::Result<To> Forward(const kb::Result<From>& from) noexcept {
  return kb::Result<To>::Failure(from.Error(), from.Detail());
}

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_DRIVER_RESULT_HPP
