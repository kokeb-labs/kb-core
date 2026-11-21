// A bare integer is not a duration: the constructors are explicit.
// EXPECT: (conversion|no viable|could not convert|cannot convert|explicit)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/time.hpp"

kb::Milliseconds Use() {
  const kb::Milliseconds ms = 5U;
  return ms;
}
