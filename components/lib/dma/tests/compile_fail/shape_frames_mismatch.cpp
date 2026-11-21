// A handler ring whose frame count differs from the board's stream declaration is a build error.
//
// EXPECT: frame count differs from the board declaration
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/dma/assignment.hpp"

struct Decl {
  static constexpr kb::dma::StreamShape kShape{2U, 2U, 4U};
};
struct Scan {
  uint16_t ranks[4];
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireShape<Decl, Scan, 4U>();
  return true;
}();
