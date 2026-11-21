// A Result cannot hold a reference: the value must be an object type.
// EXPECT: (constraints not satisfied|constraint|ResultValue)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/result.hpp"

int Use() {
  static int x = 1;
  return kb::Result<int&>::Success(x).IsOk() ? 1 : 0;
}
