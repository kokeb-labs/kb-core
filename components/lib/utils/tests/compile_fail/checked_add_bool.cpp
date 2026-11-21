// Checked arithmetic refuses bool and character types.
// EXPECT: (constraints not satisfied|constraint|no matching)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/numeric.hpp"

bool Use() {
  bool out = false;
  return kb::utils::CheckedAdd(true, false, out);
}
