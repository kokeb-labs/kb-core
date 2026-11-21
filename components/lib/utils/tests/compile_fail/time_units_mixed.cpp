// Milliseconds are not Microseconds: no implicit conversion between duration types.
// EXPECT: (conversion|no viable|could not convert|cannot convert|no matching)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/time.hpp"

kb::Microseconds Use() {
  const kb::Milliseconds ms{5U};
  const kb::Microseconds us = ms;
  return us;
}
