// A driver frame larger than a bus without DMA can carry is a build error naming the fix.
//
// EXPECT: exceeds what this bus carries
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/dma/assignment.hpp"

struct Bus {
  static constexpr kb::dma::BusLimits kBus{false, 0U, 64U, 4U, 4U};
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireBusCarries<Bus, 1028U>();
  return true;
}();
