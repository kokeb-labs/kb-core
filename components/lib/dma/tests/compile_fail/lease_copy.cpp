// A Lease cannot be copied: two copies could both end the transfer.
//
// EXPECT: (deleted|copy constructor)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

void Use(kbt::Lease& a) {
  kbt::Lease b{a};
  static_cast<void>(b);
}
