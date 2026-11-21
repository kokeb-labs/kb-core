/**
 * @file uart.hpp
 * @brief The UART contract: `kb::iface::UartPort` and `UartPortRef`; the optional `UartBaudControl`
 * and `UartTxDrain`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status EnsureReady() noexcept` | Brings the port up if it is not; idempotent |
 * | `kb::Status Write(std::span<const uint8_t> data) noexcept` | Queues every byte for sending
 * (bounded by the port's timeout) | | `kb::Result<std::size_t> Read(std::span<uint8_t> out,
 * kb::Milliseconds timeout) noexcept` | Bytes received into `out`, waiting at most `timeout` for
 * the first; a count below `out.size()` is not a failure | | `kb::Status FlushInput() noexcept` |
 * Drops everything received and not yet read |
 *
 * Optional: `kb::Status SetBaud(uint32_t baud) noexcept`, `kb::Status WaitTxDone(kb::Milliseconds)
 * noexcept`.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_UART_HPP
#define KB_INTERFACES_UART_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface {

/** @brief A UART (or RS-485, or single-wire) port. */
template <typename U>
concept UartPort =
    requires(U& port, std::span<const uint8_t> tx, std::span<uint8_t> rx, kb::Milliseconds t) {
      { port.EnsureReady() } noexcept -> std::same_as<kb::Status>;
      { port.Write(tx) } noexcept -> std::same_as<kb::Status>;
      { port.Read(rx, t) } noexcept -> std::same_as<kb::Result<std::size_t>>;
      { port.FlushInput() } noexcept -> std::same_as<kb::Status>;
    };

/** @brief A UART whose baud rate can change at run time. */
template <typename U>
concept UartBaudControl = UartPort<U> && requires(U& port, uint32_t baud) {
  { port.SetBaud(baud) } noexcept -> std::same_as<kb::Status>;
};

/** @brief A UART that can wait until its last byte has left the wire (RS-485 direction switching).
 */
template <typename U>
concept UartTxDrain = UartPort<U> && requires(U& port, kb::Milliseconds t) {
  { port.WaitTxDone(t) } noexcept -> std::same_as<kb::Status>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_UART_HPP
