// A contract member returns exactly the contract's type: bool is not kb::Status.
// EXPECT: (static assertion failed|static_assert|constraints not satisfied)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/interfaces/gpio.hpp"

struct BoolPin {
  bool Set(kb::iface::Level level) noexcept {
    return level == kb::iface::Level::High;
  }
  bool Toggle() noexcept {
    return true;
  }
};
static_assert(kb::iface::GpioOutput<BoolPin>, "must fail: Set returns bool");
