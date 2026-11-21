// A sleep takes a typed duration, never a bare integer.
// EXPECT: (no matching|no viable|could not convert|cannot convert|ambiguous)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/rtos/time.hpp"

void Use() {
  kb::rtos::SleepFor(10U);
}
