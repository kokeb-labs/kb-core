// OwnerAccess<H> on a bus declared ThreadModel::Guarded does not compile (ADR-0009).
// EXPECT: (needs a BusDomain<Bus, ThreadModel::Owner>|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(GuardedDomain& domain) {
  kb::handlers::OwnerAccess<Toy<false>, GuardedDomain> io{domain};
  return io.Create().IsOk();
}
