// A cache-hook type without Clean, Invalidate and Barrier cannot be lent with.
//
// EXPECT: (constraints not satisfied|CacheOps)
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "region_support.hpp"

struct Half {
  void Clean(std::span<const std::byte>) noexcept {}
};

struct Tag {
  kb::dma::PortKey<Tag> Key() const noexcept {
    return kb::dma::PortKey<Tag>{};
  }
};

kb::Status Use(Tag& t, kbt::Buf& b, kbt::Lease& l, Half& h) {
  return b.Lend(t.Key(), kb::dma::Direction::ToDevice, 4U, h, l);
}
