// A BusHold is move-only: a copy would release the bus twice (ADR-0016).
// EXPECT: (deleted|call to implicitly-deleted|use of deleted function)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(GuardedDomain& domain) {
  const kb::handlers::BusHold<GuardedDomain> hold = domain.Hold(kb::Milliseconds{1U});
  const kb::handlers::BusHold<GuardedDomain> copy = hold;
  return copy.OwnsLock();
}
