// The bus of a temporary hold cannot be kept: the hold is released at the end of the expression,
// so using the bus afterwards would be a use after release (ADR-0015).
// EXPECT: (deleted|use of deleted function)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "toy_handler.hpp"

kb::Status Use(OwnerDomain& domain) {
  Bus* bus = domain.Hold(kb::Milliseconds{0U}).Bus();
  return bus->EnsureReady();
}
