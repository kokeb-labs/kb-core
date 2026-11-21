// Guarded<H> on a bus declared ThreadModel::Owner does not compile (ADR-0009: one model per bus).
// EXPECT: (needs a BusDomain<Bus, ThreadModel::Guarded>|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(OwnerDomain& domain) {
  kb::handlers::Guarded<Toy<false>, OwnerDomain> io{domain};
  return io.Create().IsOk();
}
