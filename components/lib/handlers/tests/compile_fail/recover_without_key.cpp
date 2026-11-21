// Recover() (and every mutating call) needs a ServiceKey, which only the facades can make: code
// that holds a handler directly cannot recover or start it off the owner task or the guarded locks.
// EXPECT: (private|inaccessible|ServiceKey)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

bool Use(Toy<false>& handler) {
  const kb::handlers::ServiceKey key{};
  return handler.Recover(key, kb::Instant{}).IsOk();
}
