// A chip that must be serviced continuously (kRequiresOwner) cannot be wrapped in Guarded<H>.
// EXPECT: (serviced continuously|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(GuardedDomain& domain) {
  kb::handlers::Guarded<Toy<true>, GuardedDomain> io{domain};
  return io.Create().IsOk();
}
