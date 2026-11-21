// A stream element whose size differs from the board declaration is a build error.
//
// EXPECT: differs from the board declaration
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Elem {
  char bytes[6];
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireElementBytes<Elem, 8U>();
  return true;
}();
