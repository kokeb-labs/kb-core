// A fixed-extent load refuses a span of the wrong length at compile time.
// EXPECT: (no matching|no viable|could not convert|cannot convert|conversion)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <span>

#include "kb/utils/endian.hpp"

uint32_t Use() {
  const std::array<uint8_t, 3> bytes{1U, 2U, 3U};
  return kb::utils::LoadBe32(std::span<const uint8_t, 3>{bytes});
}
