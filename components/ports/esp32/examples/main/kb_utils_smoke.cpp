// kb::utils on an ESP32 target: CRC check values, byte packing, checked arithmetic and wrap-safe
// time, logged once at start-up. Proves the part builds with the ESP-IDF toolchain and runs without
// heap or exceptions. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>

#include "esp_log.h"
#include "kb/utils/utils.hpp"

namespace {

constexpr const char* kTag = "kb_utils_smoke";
constexpr std::array<uint8_t, 9> kCheck{0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                                        0x36U, 0x37U, 0x38U, 0x39U};

bool RunChecks() {
  bool ok = kb::utils::Crc32Ieee::Compute(kCheck) == 0xCBF43926UL;
  ok = ok && (kb::utils::Crc16Modbus::Compute(kCheck) == 0x4B37U);
  std::array<uint8_t, 4> buffer{};
  kb::utils::ByteWriter writer{buffer};
  ok = ok && writer.WriteBe32(0x01020304UL);
  ok = ok && (kb::utils::LoadLe32(std::span<const uint8_t, 4>{buffer}) == 0x04030201UL);
  ok = ok && (kb::utils::SaturatingAdd<uint8_t>(250U, 10U) == 255U);
  ok = ok && kb::utils::HasElapsed(5U, 0xFFFFFFFBU, 10U);
  return ok;
}

} // namespace

extern "C" void app_main(void) {
  kb::utils::FixedString<48> line{};
  const bool ok = RunChecks();
  static_cast<void>(line.Append("kb::utils checks: "));
  static_cast<void>(line.Append(ok ? "pass" : "FAIL"));
  ESP_LOGI(kTag, "%s", line.CStr());
}
