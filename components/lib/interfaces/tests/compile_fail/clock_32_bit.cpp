// The monotonic clock is the 64-bit kb::Instant; a 32-bit microsecond counter does not satisfy it.
// EXPECT: (static assertion failed|static_assert|constraints not satisfied)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/interfaces/time.hpp"

struct Clock32 {
  uint32_t Now() const noexcept {
    return 0U;
  }
};
static_assert(kb::iface::MonotonicClock<Clock32>, "must fail: Now returns uint32_t");
