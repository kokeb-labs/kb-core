// A frame view is move-only: two views of one slot would release it twice.
//
// EXPECT: (deleted|copy constructor)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Frame {
  char bytes[4];
};
using S = kb::dma::Stream<Frame, 2U, kbt::Cached>;

void Use(S::FrameView& a) {
  S::FrameView b{a};
  static_cast<void>(b);
}
