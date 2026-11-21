// A DMA master that cannot reach the region (tightly coupled memory, for one) is a build error.
//
// EXPECT: cannot reach this memory region
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Tcm {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::Coherent;
  static constexpr std::size_t kLineBytes = 1U;
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr kb::dma::MasterSet kMasters = kb::dma::MasterBit(0U);
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireReachable<kb::dma::MasterBit(1U), Tcm>();
  return true;
}();
