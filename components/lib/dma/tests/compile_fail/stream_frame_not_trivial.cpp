// A stream frame is a plain copyable value.
//
// EXPECT: (constraints not satisfied|StreamFrame)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct NotTrivial {
  NotTrivial() = default;
  NotTrivial(const NotTrivial&) {}
};

kb::dma::Stream<NotTrivial, 2U, kbt::Cached> g_stream;
