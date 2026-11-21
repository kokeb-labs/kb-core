// kb::utils on a PC: CRC check values, byte packing, checked arithmetic and wrap-safe time. The
// same checks run on a target in the ESP32 port's kb_utils_smoke example. Documentation snippets
// are taken from this file (docs/parts/utils.md). SPDX-License-Identifier: MIT Copyright (c) 2026
// The kb-core authors
#include <array>
#include <cstdint>
#include <cstdio>

#include "kb/utils/utils.hpp"

namespace {

// --8<-- [start:checks]
constexpr std::array<uint8_t, 9> kCheck{0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                                        0x36U, 0x37U, 0x38U, 0x39U};

bool RunChecks() {
  // The CRC catalogue check value is the CRC of the ASCII digits "123456789".
  bool ok = kb::utils::Crc32Ieee::Compute(kCheck) == 0xCBF43926UL;
  ok = ok && (kb::utils::Crc16Modbus::Compute(kCheck) == 0x4B37U);
  // Byte packing into a caller-owned buffer, read back little-endian.
  std::array<uint8_t, 4> buffer{};
  kb::utils::ByteWriter writer{buffer};
  ok = ok && writer.WriteBe32(0x01020304UL);
  ok = ok && (kb::utils::LoadLe32(std::span<const uint8_t, 4>{buffer}) == 0x04030201UL);
  // Saturating arithmetic and a time comparison that survives the 32-bit counter wrapping.
  ok = ok && (kb::utils::SaturatingAdd<uint8_t>(250U, 10U) == 255U);
  ok = ok && kb::utils::HasElapsed(5U, 0xFFFFFFFBU, 10U);
  return ok;
}
// --8<-- [end:checks]

} // namespace

int main() {
  const bool ok = RunChecks();
  static_cast<void>(std::puts(ok ? "kb::utils checks: pass" : "kb::utils checks: FAIL"));
  return ok ? 0 : 1;
}
