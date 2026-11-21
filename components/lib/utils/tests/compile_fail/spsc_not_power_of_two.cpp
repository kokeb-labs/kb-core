// An SpscRing capacity must be a power of two.
// EXPECT: (constraints not satisfied|constraint|IsPowerOfTwo|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/utils/ring_buffer.hpp"

kb::utils::SpscRing<uint8_t, 6> ring{};
