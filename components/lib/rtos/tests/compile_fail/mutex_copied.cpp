// RTOS objects are not copyable: a copy would alias the native object.
// EXPECT: (deleted|use of deleted)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/rtos/mutex.hpp"

void Use(kb::rtos::Mutex& m) {
  kb::rtos::Mutex copy = m;
  static_cast<void>(copy.IsCreated());
}
