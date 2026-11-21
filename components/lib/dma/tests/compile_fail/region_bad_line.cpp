// A region whose line size is not a power of two is not a Region.
//
// EXPECT: (constraints not satisfied|Region)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct BadLine {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::Coherent;
  static constexpr std::size_t kLineBytes = 3U;
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr kb::dma::MasterSet kMasters = 1U;
};

kb::dma::Buffer<BadLine, 8U> g_buffer;
