/**
 * @file can.hpp
 * @brief The CAN contract: `kb::iface::CanBus` (a controller on one bus) and `CanBusRef`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status EnsureReady() noexcept` | Brings the controller onto the bus if it is not;
 * idempotent | | `kb::Status Send(const CanFrame& frame, kb::Milliseconds timeout) noexcept` |
 * Queues the frame, waiting at most `timeout` for a transmit slot | | `kb::Status Receive(CanFrame&
 * frame, kb::Milliseconds timeout) noexcept` | Takes the oldest received frame, waiting at most
 * `timeout` |
 *
 * Results include `Timeout`, `BusError` (bus-off, error passive) and `InvalidArgument` (dlc above
 * 8).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_CAN_HPP
#define KB_INTERFACES_CAN_HPP

#include <concepts>

#include "kb/interfaces/types.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface {

/** @brief A CAN controller on one bus. */
template <typename C>
concept CanBus = requires(C& can, const CanFrame& tx, CanFrame& rx, kb::Milliseconds t) {
  { can.EnsureReady() } noexcept -> std::same_as<kb::Status>;
  { can.Send(tx, t) } noexcept -> std::same_as<kb::Status>;
  { can.Receive(rx, t) } noexcept -> std::same_as<kb::Status>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_CAN_HPP
