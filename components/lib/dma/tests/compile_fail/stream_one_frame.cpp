// A stream needs at least two frames: with one, the producer cannot publish while the consumer
// holds it.
//
// EXPECT: at least two frames
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Frame {
  char bytes[4];
};

kb::dma::Stream<Frame, 1U, kbt::Cached> g_stream;
