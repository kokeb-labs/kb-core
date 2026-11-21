// A Thread's Derived class must provide Setup, Step and Cleanup, each noexcept.
// EXPECT: (Setup, Step and Cleanup|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/rtos/thread.hpp"

class NoStep : public kb::rtos::Thread<NoStep, 2048U> {
public:
  bool Setup() noexcept {
    return true;
  }
  void Cleanup() noexcept {}
};

bool Use(NoStep& t) {
  return t.Start(kb::rtos::ThreadConfig{}).IsOk();
}
