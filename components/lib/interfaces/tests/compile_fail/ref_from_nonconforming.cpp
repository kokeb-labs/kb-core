// A *Ref is made only from a type that satisfies its contract.
// EXPECT: (no matching|constraints not satisfied|candidate template ignored|could not convert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/interfaces/refs.hpp"

struct HalfBus {
  kb::Status EnsureReady() noexcept {
    return kb::Status::Ok;
  }
};

void Use(HalfBus& bus) {
  kb::iface::I2cBusRef ref{bus};
  static_cast<void>(ref);
}
