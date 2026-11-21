// Only the port named by the key's tag can make a PortKey: application code cannot lend or
// complete.
//
// EXPECT: (private|inaccessible|protected)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Port;

kb::dma::PortKey<Port> Forge() {
  return kb::dma::PortKey<Port>{};
}
