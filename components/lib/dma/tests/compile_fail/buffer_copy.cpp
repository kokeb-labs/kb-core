// A Buffer cannot be copied or moved: the hardware knows its address.
//
// EXPECT: (deleted|copy constructor)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

void Use(const kbt::Buf& a) {
  kbt::Buf b{a};
  static_cast<void>(b);
}
