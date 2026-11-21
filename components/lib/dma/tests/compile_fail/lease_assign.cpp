// A Lease cannot be assigned over: the lease it would replace would be dropped unsettled.
//
// EXPECT: (deleted|assign)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

void Use(kbt::Lease& a, kbt::Lease&& b) {
  a = static_cast<kbt::Lease&&>(b);
}
