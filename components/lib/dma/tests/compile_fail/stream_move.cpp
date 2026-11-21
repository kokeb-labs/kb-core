// A stream is not movable: the interrupt holds its address.
//
// EXPECT: (deleted|move constructor)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Frame {
  char bytes[4];
};
using S = kb::dma::Stream<Frame, 2U, kbt::Cached>;

void Use(S& a) {
  S b{static_cast<S&&>(a)};
  static_cast<void>(b);
}
