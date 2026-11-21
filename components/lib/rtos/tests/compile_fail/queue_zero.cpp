// A queue needs a capacity of at least one.
// EXPECT: (constraints not satisfied|constraint|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/rtos/queue.hpp"

kb::rtos::Queue<uint8_t, 0> q{};
