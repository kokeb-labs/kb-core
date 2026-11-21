/**
 * @file adc.hpp
 * @brief The ADC contract: `kb::iface::AdcChannel` (one converted input) and `AdcChannelRef`.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Result<uint32_t> ReadRaw() noexcept` | One conversion, in counts |
 * | `kb::Result<int32_t> ReadMillivolts() noexcept` | One conversion, calibrated by the port, in
 * millivolts | | `uint32_t FullScale() const noexcept` | The largest raw count (4095 for 12 bits) |
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_ADC_HPP
#define KB_INTERFACES_ADC_HPP

#include <concepts>
#include <cstdint>

#include "kb/utils/result.hpp"

namespace kb::iface {

/** @brief One ADC input. */
template <typename A>
concept AdcChannel = requires(A& adc, const A& cadc) {
  { adc.ReadRaw() } noexcept -> std::same_as<kb::Result<uint32_t>>;
  { adc.ReadMillivolts() } noexcept -> std::same_as<kb::Result<int32_t>>;
  { cadc.FullScale() } noexcept -> std::same_as<uint32_t>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_ADC_HPP
