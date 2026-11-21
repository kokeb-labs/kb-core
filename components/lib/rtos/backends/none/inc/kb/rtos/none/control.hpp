/**
 * @file control.hpp
 * @brief Test control of the `none` backend (no scheduler): its fake tick clock and its
 * critical-section depth.
 *
 * The `none` backend runs everything in one context and is deterministic: time advances only when
 * something waits (a sleep, or a bounded wait that cannot succeed) or when a test calls
 * AdvanceTicks(). Software timers fire while time advances. Threads cannot be started
 * (Status::Unsupported).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_NONE_CONTROL_HPP
#define KB_RTOS_NONE_CONTROL_HPP

#include <cstdint>

namespace kb::rtos::none {

/** @brief The fake tick rate: 1000 Hz, one tick per millisecond. */
inline constexpr uint32_t kTickRateHz = 1000U;

/** @brief Advances the fake clock by `ticks`, firing due timers tick by tick. */
void AdvanceTicks(uint32_t ticks) noexcept;

/** @brief Sets the fake 64-bit tick count (for wrap tests); timers are not fired. */
void SetTicks(uint64_t ticks) noexcept;

/** @brief Current nesting depth of critical sections. */
[[nodiscard]] uint32_t CriticalDepth() noexcept;

} // namespace kb::rtos::none

#endif // KB_RTOS_NONE_CONTROL_HPP
