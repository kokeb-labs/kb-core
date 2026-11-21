// A UART read is always bounded: Read without a timeout does not satisfy UartPort.
// EXPECT: (static assertion failed|static_assert|constraints not satisfied)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/uart.hpp"

struct UnboundedUart {
  kb::Status EnsureReady() noexcept {
    return kb::Status::Ok;
  }
  kb::Status Write(std::span<const uint8_t> tx) noexcept {
    static_cast<void>(tx);
    return kb::Status::Ok;
  }
  kb::Result<std::size_t> Read(std::span<uint8_t> rx) noexcept {
    return kb::Result<std::size_t>::Success(rx.size());
  }
  kb::Status FlushInput() noexcept {
    return kb::Status::Ok;
  }
};
static_assert(kb::iface::UartPort<UnboundedUart>, "must fail: Read takes no timeout");
