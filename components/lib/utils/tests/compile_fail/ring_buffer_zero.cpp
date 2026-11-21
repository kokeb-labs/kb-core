// A ring buffer needs a capacity of at least one.
// EXPECT: (constraints not satisfied|constraint|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/utils/ring_buffer.hpp"

kb::utils::RingBuffer<uint8_t, 0> r{};
