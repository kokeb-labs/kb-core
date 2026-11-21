// The two streams of one peripheral at different interrupt priorities is a build error (completion
// and error could preempt each other).
//
// EXPECT: different interrupt priorities
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/dma/assignment.hpp"

struct Rx {
  static constexpr kb::dma::StreamAssignment kAssignment{1U, 0U, 37U,
                                                         kb::dma::ChannelPriority::High, 5U};
};
struct Tx {
  static constexpr kb::dma::StreamAssignment kAssignment{1U, 1U, 38U,
                                                         kb::dma::ChannelPriority::High, 6U};
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireSamePriority<Rx, Tx>();
  return true;
}();
