// A hot-path poller over a port whose Start() waits (AsyncFromSync) does not compile: say
// AsyncUse::MayBlock, or use a native asynchronous port (ADR-0016).
// EXPECT: (Start\(\) waits for the transfer|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/handlers/common/async_poller.hpp"
#include "toy_handler.hpp"

struct Clock {
  [[nodiscard]] kb::Instant Now() const noexcept {
    return kb::Instant{0U};
  }
};

bool Use(Bus& bus, Clock& clock) {
  kb::iface::AsyncFromSync<Bus, Clock> adapter{bus, clock};
  kb::handlers::AsyncPoller<kb::iface::AsyncFromSync<Bus, Clock>, 4U> poller{adapter};
  return poller.InFlight();
}
