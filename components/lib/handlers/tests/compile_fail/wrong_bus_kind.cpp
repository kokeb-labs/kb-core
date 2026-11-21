// A handler whose chip is on SPI cannot be put on an I2C bus domain.
// EXPECT: (not on this kind of bus|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(GuardedDomain& domain) {
  kb::handlers::Guarded<Toy<false, kb::handlers::BusKind::Spi>, GuardedDomain> io{domain};
  return io.Create().IsOk();
}
