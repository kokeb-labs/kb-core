// Tests of the CRC presets against their catalogue check values, and of incremental use (crc.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <span>

#include "kb/utils/crc.hpp"
#include "kb_test.hpp"

namespace {

constexpr std::array<uint8_t, 9> kCheck{'1', '2', '3', '4', '5', '6', '7', '8', '9'};

template <typename CrcT>
auto Split(std::size_t at) {
  CrcT crc{};
  const std::span<const uint8_t> all{kCheck};
  crc.Update(all.first(at));
  crc.Update(all.subspan(at));
  return crc.Value();
}

// The catalogue check values hold at compile time as well.
static_assert(kb::utils::Crc8Smbus::Compute(kCheck) == 0xF4U);
static_assert(kb::utils::Crc32Ieee::Compute(kCheck) == 0xCBF43926UL);

} // namespace

KB_TEST("crc: every preset gives its catalogue check value") {
  const std::span<const uint8_t> data{kCheck};
  KB_CHECK_EQ(kb::utils::Crc8Smbus::Compute(data), 0xF4U);
  KB_CHECK_EQ(kb::utils::Crc8Sensirion::Compute(data), 0xF7U);
  KB_CHECK_EQ(kb::utils::Crc8Maxim::Compute(data), 0xA1U);
  KB_CHECK_EQ(kb::utils::Crc8SaeJ1850::Compute(data), 0x4BU);
  KB_CHECK_EQ(kb::utils::Crc16CcittFalse::Compute(data), 0x29B1U);
  KB_CHECK_EQ(kb::utils::Crc16Xmodem::Compute(data), 0x31C3U);
  KB_CHECK_EQ(kb::utils::Crc16Kermit::Compute(data), 0x2189U);
  KB_CHECK_EQ(kb::utils::Crc16Modbus::Compute(data), 0x4B37U);
  KB_CHECK_EQ(kb::utils::Crc32Ieee::Compute(data), 0xCBF43926UL);
  KB_CHECK_EQ(kb::utils::Crc32C::Compute(data), 0xE3069283UL);
  KB_CHECK_EQ(kb::utils::Crc32Mpeg2::Compute(data), 0x0376E6E7UL);
}

KB_TEST("crc: the Sensirion example word") {
  const std::array<uint8_t, 2> word{0xBEU, 0xEFU};
  KB_CHECK_EQ(kb::utils::Crc8Sensirion::Compute(word), 0x92U);
}

KB_TEST("crc: feeding in pieces equals one call, and Reset starts over") {
  for (std::size_t at = 0U; at <= kCheck.size(); ++at) {
    KB_CHECK_EQ(Split<kb::utils::Crc16Modbus>(at), 0x4B37U);
    KB_CHECK_EQ(Split<kb::utils::Crc32Ieee>(at), 0xCBF43926UL);
    KB_CHECK_EQ(Split<kb::utils::Crc8Sensirion>(at), 0xF7U);
    KB_CHECK_EQ(Split<kb::utils::Crc16Xmodem>(at), 0x31C3U);
  }
  kb::utils::Crc16CcittFalse crc{};
  crc.Update(std::array<uint8_t, 1>{0x00U});
  crc.Reset();
  crc.Update(kCheck);
  KB_CHECK_EQ(crc.Value(), 0x29B1U);
}

KB_TEST("crc: empty input gives init xor xorout") {
  const std::span<const uint8_t> none{};
  KB_CHECK_EQ(kb::utils::Crc32Ieee::Compute(none), 0x00000000UL);
  KB_CHECK_EQ(kb::utils::Crc16CcittFalse::Compute(none), 0xFFFFU);
  KB_CHECK_EQ(kb::utils::Crc8SaeJ1850::Compute(none), 0x00U);
}
