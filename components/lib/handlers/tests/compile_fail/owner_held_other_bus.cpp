// Held calls of an owner-bus handler take a hold of that handler's bus type: a hold of another bus
// does not compile (ADR-0015).
// EXPECT: (no matching|cannot bind|cannot convert|could not convert|no viable conversion)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

using SpiTag = kb::handlers::BusTag<kb::handlers::BusKind::Spi>;
using SpiDomain = kb::handlers::BusDomain<SpiTag, kb::handlers::ThreadModel::Owner>;

bool Use(OwnerDomain& domain, SpiDomain& spi) {
  kb::handlers::OwnerAccess<Toy<false>, OwnerDomain> io{domain};
  kb::handlers::BusHold<SpiDomain> other = spi.Hold(kb::Milliseconds{0U});
  const kb::handlers::OwnerHeld<kb::handlers::OwnerAccess<Toy<false>, OwnerDomain>> held =
      io.Held(other);
  return held.OwnsLock();
}
