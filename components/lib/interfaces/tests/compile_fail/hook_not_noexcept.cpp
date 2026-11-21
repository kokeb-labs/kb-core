// Every contract member is noexcept: a throwing-capable member does not satisfy the concept.
// EXPECT: (static assertion failed|static_assert|constraints not satisfied)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "kb/interfaces/spi.hpp"

struct LooseSpi {
  kb::Status EnsureReady() noexcept {
    return kb::Status::Ok;
  }
  kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) {
    static_cast<void>(tx);
    static_cast<void>(rx);
    return kb::Status::Ok;
  }
};
static_assert(kb::iface::SpiDevice<LooseSpi>, "must fail: Transfer is not noexcept");
