// Instants are never ordered (no half-range compare): use ElapsedUs or a Deadline.
// EXPECT: (no match for|invalid operands|operator<)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/time.hpp"

bool Use(kb::Instant a, kb::Instant b) {
  return a < b;
}
