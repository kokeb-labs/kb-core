// A hold that is discarded is released at once: Hold() may not be called for nothing (ADR-0015).
// EXPECT: (nodiscard|ignoring return value|unused-result)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

void Use(GuardedDomain& domain) {
  domain.Hold(kb::Milliseconds{1U});
}
