// A Queue carries trivially copyable items only.
// EXPECT: (constraints not satisfied|constraint|QueueItem|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/rtos/queue.hpp"

struct NotTrivial {
  NotTrivial() = default;
  NotTrivial(const NotTrivial&) {}
};
kb::rtos::Queue<NotTrivial, 2> q{};
