/**
 * @file gpio.hpp
 * @brief The pin contracts: `GpioOutput`, `GpioInput`, `GpioInterrupt`, and `GpioOutputRef`,
 * `GpioInputRef`.
 *
 * | Concept | Members |
 * | --- | --- |
 * | `GpioOutput` | `kb::Status Set(Level) noexcept`, `kb::Status Toggle() noexcept` |
 * | `GpioInput` | `kb::Status Get(Level& out) noexcept` |
 * | `GpioInterrupt` | `GpioInput` plus `kb::Status EnableInterrupt(Edge, GpioCallback, void*
 * context) noexcept`, `kb::Status DisableInterrupt() noexcept` |
 *
 * Levels are electrical; whether low means "asserted" is the HAL's configuration. An interrupt
 * callback runs in interrupt context: it may only use `...FromIsr` calls of kb::rtos.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_GPIO_HPP
#define KB_INTERFACES_GPIO_HPP

#include <concepts>

#include "kb/interfaces/types.hpp"
#include "kb/utils/status.hpp"

namespace kb::iface {

/** @brief A GPIO interrupt callback: a plain function with a context pointer, run in interrupt
 * context. */
using GpioCallback = void (*)(void* context) noexcept;

/** @brief A pin this code drives. */
template <typename P>
concept GpioOutput = requires(P& pin, Level level) {
  { pin.Set(level) } noexcept -> std::same_as<kb::Status>;
  { pin.Toggle() } noexcept -> std::same_as<kb::Status>;
};

/** @brief A pin this code reads. */
template <typename P>
concept GpioInput = requires(P& pin, Level& out) {
  { pin.Get(out) } noexcept -> std::same_as<kb::Status>;
};

/** @brief An input pin that can call a function on an edge. */
template <typename P>
concept GpioInterrupt =
    GpioInput<P> && requires(P& pin, Edge edge, GpioCallback fn, void* context) {
      { pin.EnableInterrupt(edge, fn, context) } noexcept -> std::same_as<kb::Status>;
      { pin.DisableInterrupt() } noexcept -> std::same_as<kb::Status>;
    };

} // namespace kb::iface

#endif // KB_INTERFACES_GPIO_HPP
