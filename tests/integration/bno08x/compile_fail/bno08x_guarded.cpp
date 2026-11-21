// bno08x must be serviced continuously (kRequiresOwner, ADR-0009): Guarded<Handler> fails.
// EXPECT: (serviced continuously|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/handlers/bindings/bno08x.hpp"
#include "kb/interfaces/host/host.hpp"

using Bus = kb::iface::host::FakeI2cBus;
using Domain = kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded>;

bool Use(Domain& domain, kb::handlers::Guarded<kb::handlers::bno08x::Handler<Bus>, Domain>& io) {
  static_cast<void>(domain);
  return io.Create().IsOk();
}
