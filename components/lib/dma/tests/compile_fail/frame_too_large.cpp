// A driver frame above the port's DMA limit is a build error.
//
// EXPECT: exceeds the port's DMA frame limit
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireFrameFits<4096U, 1024U>();
  return true;
}();
