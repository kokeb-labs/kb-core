/**
 * @file spi.hpp
 * @brief The SPI contract: `kb::iface::SpiDevice` (one chip select on a bus, mode and clock fixed
 * by the HAL) and `SpiDeviceRef`; the optional `SpiClockControl`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status EnsureReady() noexcept` | Brings the device up if it is not; idempotent |
 * | `kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept` | One
 * full-duplex frame with chip select held throughout |
 *
 * Either span may be empty (write only, or read only with zeros clocked out); when both are given
 * they have the same length, otherwise the result is `InvalidArgument` and nothing is clocked.
 * Optional: `kb::Status SetClockHz(uint32_t hz) noexcept` (SpiClockControl).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_SPI_HPP
#define KB_INTERFACES_SPI_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/utils/status.hpp"

namespace kb::iface {

/** @brief One SPI device: a chip select on a bus with its mode and clock. */
template <typename D>
concept SpiDevice = requires(D& device, std::span<const uint8_t> tx, std::span<uint8_t> rx) {
  { device.EnsureReady() } noexcept -> std::same_as<kb::Status>;
  { device.Transfer(tx, rx) } noexcept -> std::same_as<kb::Status>;
};

/** @brief An SPI device whose clock can change at run time. */
template <typename D>
concept SpiClockControl = SpiDevice<D> && requires(D& device, uint32_t hz) {
  { device.SetClockHz(hz) } noexcept -> std::same_as<kb::Status>;
};

/** @brief True when a Transfer's spans are acceptable: either empty, or both of one length. */
[[nodiscard]] constexpr bool SpiSpansValid(std::span<const uint8_t> tx,
                                           std::span<uint8_t> rx) noexcept {
  return tx.empty() || rx.empty() || (tx.size() == rx.size());
}

} // namespace kb::iface

#endif // KB_INTERFACES_SPI_HPP
