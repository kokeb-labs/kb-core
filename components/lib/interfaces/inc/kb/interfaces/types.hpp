/**
 * @file types.hpp
 * @brief The vocabulary of the kb::iface contracts: pin levels and edges, the I2C failure kinds,
 * the CAN frame, log levels and the PWM duty.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_TYPES_HPP
#define KB_INTERFACES_TYPES_HPP

#include <array>
#include <cstdint>

namespace kb::iface {

/** @brief The electrical level of a pin. Polarity (active low or high) is HAL configuration, never
 * here. */
enum class Level : uint8_t {
  Low = 0,  ///< Logic low.
  High = 1, ///< Logic high.
};

/** @brief The edges a GPIO interrupt fires on. */
enum class Edge : uint8_t {
  Rising = 0,  ///< Low to high.
  Falling = 1, ///< High to low.
  Both = 2,    ///< Either.
};

/** @brief Why the last I2C transfer of a bus failed (`None` after a success). */
enum class I2cFailure : uint8_t {
  None = 0,        ///< The last transfer succeeded.
  AddressNack = 1, ///< No device acknowledged the address.
  DataNack = 2,    ///< The device acknowledged its address, then refused a data byte.
  Bus = 3,         ///< Arbitration lost or another bus error.
  Timeout = 4,     ///< The transfer did not finish in time (clock stretching too long).
  BusStuck = 5,    ///< SDA or SCL held low; RecoverBus() is needed.
};

/** @brief A classic CAN frame (CAN 2.0A/B; CAN FD later). */
struct CanFrame {
  uint32_t id{0U};               ///< 11-bit or 29-bit identifier.
  bool extended{false};          ///< 29-bit identifier.
  bool remote{false};            ///< Remote transmission request.
  uint8_t dlc{0U};               ///< Data length, 0 to 8.
  std::array<uint8_t, 8> data{}; ///< Payload; bytes past `dlc` are ignored.
};

/** @brief The largest CAN 2.0 data length. */
inline constexpr uint8_t kCanMaxDlc = 8U;

/** @brief Severity of a log line. */
enum class LogLevel : uint8_t {
  Error = 0,   ///< Something failed.
  Warning = 1, ///< Something is wrong but handled.
  Info = 2,    ///< Normal operation worth recording.
  Debug = 3,   ///< Detail for bench work.
};

/** @brief A PWM duty cycle in hundredths of a percent: 0 (always low) to 10000 (always high). */
class Duty {
public:
  constexpr Duty() noexcept = default;
  /** @brief `permyriad` hundredths of a percent, clamped to 10000. */
  constexpr explicit Duty(uint16_t permyriad) noexcept
      : permyriad_((permyriad > kFull) ? kFull : permyriad) {}
  /** @brief Hundredths of a percent, 0 to 10000. */
  [[nodiscard]] constexpr uint16_t Permyriad() const noexcept {
    return permyriad_;
  }
  /** @brief 100 %. */
  static constexpr uint16_t kFull = 10000U;

private:
  uint16_t permyriad_{0U};
};

/** @brief Equality of two duty cycles. */
[[nodiscard]] constexpr bool operator==(Duty a, Duty b) noexcept {
  return a.Permyriad() == b.Permyriad();
}

} // namespace kb::iface

#endif // KB_INTERFACES_TYPES_HPP
