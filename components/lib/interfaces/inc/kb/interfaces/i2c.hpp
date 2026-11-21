/**
 * @file i2c.hpp
 * @brief The I2C contract: `kb::iface::I2cBus` (a controller addressed per transfer) and its
 * type-erased `I2cBusRef`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status EnsureReady() noexcept` | Brings the controller up if it is not; idempotent; `Ok`
 * when usable | | `kb::Status Write(uint8_t address, std::span<const uint8_t> data) noexcept` | One
 * write transfer (START, address, data, STOP) | | `kb::Status WriteRead(uint8_t address,
 * std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept` | Write then read with a repeated
 * START | | `kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept` | One read transfer
 * | | `I2cFailure LastFailure() const noexcept` | Why the last transfer failed (`None` after a
 * success) |
 *
 * Addresses are 7-bit (0x08 to 0x77). Every call is bounded by the controller's timeout. Results:
 * `Ok`, `NoDevice` (address NACK), `Nack` (data NACK), `BusError`, `Timeout`, `InvalidArgument`,
 * `NotReady`. A driver port adapts this with a few lines (docs/port-contracts.md).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_I2C_HPP
#define KB_INTERFACES_I2C_HPP

#include <concepts>
#include <cstdint>
#include <span>

#include "kb/interfaces/types.hpp"
#include "kb/utils/status.hpp"

namespace kb::iface {

/** @brief An I2C controller that addresses a device per transfer. */
template <typename B>
concept I2cBus = requires(B& bus, const B& cbus, uint8_t address, std::span<const uint8_t> tx,
                          std::span<uint8_t> rx) {
  { bus.EnsureReady() } noexcept -> std::same_as<kb::Status>;
  { bus.Write(address, tx) } noexcept -> std::same_as<kb::Status>;
  { bus.WriteRead(address, tx, rx) } noexcept -> std::same_as<kb::Status>;
  { bus.Read(address, rx) } noexcept -> std::same_as<kb::Status>;
  { cbus.LastFailure() } noexcept -> std::same_as<I2cFailure>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_I2C_HPP
