// A guarded handler cannot run calls under a bus hold taken first: that would take the handler
// lock while holding the bus, the reverse of ADR-0009's order. Guarded::Hold takes both in order.
// EXPECT: (no member named 'Held'|has no member named|no matching)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(GuardedDomain& domain) {
  kb::handlers::Guarded<Toy<false>, GuardedDomain> io{domain};
  kb::handlers::BusHold<GuardedDomain> bus = domain.Hold(kb::Milliseconds{1U});
  return io.Held(bus).OwnsLock();
}
