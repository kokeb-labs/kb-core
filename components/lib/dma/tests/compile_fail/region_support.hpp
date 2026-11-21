// Shared types of the compile-fail tests of kb::dma.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_DMA_TESTS_REGION_SUPPORT_HPP
#define KB_DMA_TESTS_REGION_SUPPORT_HPP

#include <cstddef>
#include <span>

#include "kb/dma/dma.hpp"

namespace kbt {

struct Cached {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::Maintained;
  static constexpr std::size_t kLineBytes = 32U;
  static constexpr std::size_t kAlignBytes = 32U;
  static constexpr kb::dma::MasterSet kMasters = kb::dma::MasterBit(0U);
};

using Buf = kb::dma::Buffer<Cached, 40U>;
using Lease = kb::dma::Lease<Cached>;

} // namespace kbt

#endif // KB_DMA_TESTS_REGION_SUPPORT_HPP
