// A buffer holds at least one byte.
//
// EXPECT: at least one byte
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

kb::dma::Buffer<kbt::Cached, 0U> g_buffer;
