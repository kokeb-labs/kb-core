/**
 * @file pwm.hpp
 * @brief The PWM contract: `kb::iface::PwmOutput` (one channel) and `PwmOutputRef`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status SetFrequencyHz(uint32_t hz) noexcept` | The period; `InvalidArgument` outside the
 * channel's range | | `kb::Status SetDuty(Duty duty) noexcept` | High time per period, 0 to 100 %
 * in steps of 0.01 % | | `kb::Status Enable(bool on) noexcept` | Starts or stops the output
 * (stopped: the idle level) |
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_PWM_HPP
#define KB_INTERFACES_PWM_HPP

#include <concepts>
#include <cstdint>

#include "kb/interfaces/types.hpp"
#include "kb/utils/status.hpp"

namespace kb::iface {

/** @brief One PWM output channel. */
template <typename P>
concept PwmOutput = requires(P& pwm, uint32_t hz, Duty duty, bool on) {
  { pwm.SetFrequencyHz(hz) } noexcept -> std::same_as<kb::Status>;
  { pwm.SetDuty(duty) } noexcept -> std::same_as<kb::Status>;
  { pwm.Enable(on) } noexcept -> std::same_as<kb::Status>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_PWM_HPP
