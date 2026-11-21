/**
 * @file status.hpp
 * @brief kb::Status, the one error vocabulary of kb-core, and its names.
 *
 * Every kb-core call that can fail reports one of these values, alone (`kb::Status`) or with a
 * value
 * (`kb::Result<T>`, result.hpp). Ports put a vendor code in the result's `Detail()`.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_STATUS_HPP
#define KB_UTILS_STATUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace kb {

/** @brief What happened. `Ok` is the only success; every other value is a failure. */
enum class Status : uint8_t {
  Ok = 0,              ///< The call did what was asked.
  InvalidArgument = 1, ///< An argument is outside what the call accepts; nothing was done.
  OutOfRange = 2,      ///< A value or index does not fit the destination.
  Timeout = 3,         ///< The bounded wait ended before the event.
  Busy = 4,            ///< The resource is held by someone else; try again.
  NotReady = 5,        ///< The object is not started, or not yet able to serve the call.
  NoDevice = 6,        ///< Nothing answered at the address (an I2C address NACK, a missing chip).
  Nack = 7,            ///< The device refused a data byte.
  BusError = 8,     ///< The bus failed: arbitration lost, a stuck line, a framing or parity error.
  Overflow = 9,     ///< Data arrived faster than it was consumed, or a result does not fit.
  Full = 10,        ///< A fixed-capacity container or queue has no free slot.
  Empty = 11,       ///< A container or queue has nothing to take.
  Unsupported = 12, ///< The port or backend does not provide this operation.
  AlreadyStarted = 13, ///< A start was asked of an object that is already started.
  CrcMismatch = 14,    ///< A received checksum does not match the data.
  Fault = 15, ///< A failure that no other value describes; `Detail()` holds the vendor code.
};

/** @brief The number of `Status` values (for tables indexed by status). */
inline constexpr uint8_t kStatusCount = 16U;

namespace detail {
/// The names of the Status values, indexed by value.
inline constexpr std::array<const char*, kStatusCount> kStatusNames{
    "Ok",          "InvalidArgument", "OutOfRange",  "Timeout",
    "Busy",        "NotReady",        "NoDevice",    "Nack",
    "BusError",    "Overflow",        "Full",        "Empty",
    "Unsupported", "AlreadyStarted",  "CrcMismatch", "Fault"};
} // namespace detail

/**
 * @brief The enumerator's name, for logs: "Ok", "Timeout", ...
 * @return A string literal; never null. An out-of-range value gives "Unknown".
 */
[[nodiscard]] constexpr const char* ToString(Status status) noexcept {
  const auto index = static_cast<std::size_t>(status);
  return (index < detail::kStatusNames.size()) ? detail::kStatusNames[index] : "Unknown";
}

/** @brief True for `Status::Ok`. */
[[nodiscard]] constexpr bool IsOk(Status status) noexcept {
  return status == Status::Ok;
}

} // namespace kb

#endif // KB_UTILS_STATUS_HPP
