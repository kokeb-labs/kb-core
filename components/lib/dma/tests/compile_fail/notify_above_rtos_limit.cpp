// A stream that signals a task from an interrupt more urgent than the RTOS syscall limit is a build
// error (the RTOS cannot be called from there).
//
// EXPECT: runs above the RTOS syscall limit
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/dma/assignment.hpp"

struct Rtos {
  static constexpr kb::dma::InterruptLimits kLimits{5U, 4U};
};
struct Stream {
  static constexpr kb::dma::StreamAssignment kAssignment{
      1U, 0U, 37U, kb::dma::ChannelPriority::High, 4U, kb::dma::Notify::Task};
};

[[maybe_unused]] constexpr bool kCheck = [] {
  kb::dma::RequireInterruptTier<Stream, Rtos>();
  return true;
}();
