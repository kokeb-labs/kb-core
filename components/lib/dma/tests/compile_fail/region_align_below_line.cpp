// The alignment unit must cover a whole cache line, or maintenance would reach a neighbour.
//
// EXPECT: (constraints not satisfied|Region)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Narrow {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::Maintained;
  static constexpr std::size_t kLineBytes = 32U;
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr kb::dma::MasterSet kMasters = 1U;
};

kb::dma::Buffer<Narrow, 8U> g_buffer;
