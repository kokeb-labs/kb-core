// A size that is not a multiple of the region's alignment unit is a build error where the unit
// matters.
//
// EXPECT: not a multiple of the region
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireWholeUnits<40U, kbt::Cached>();
  return true;
}();
