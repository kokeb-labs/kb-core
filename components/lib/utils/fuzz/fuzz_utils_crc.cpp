// Fuzz target: every table-driven CRC equals a bit-by-bit reference, in one call and split at any
// point. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "kb/utils/crc.hpp"

namespace {

// The textbook bitwise algorithm, independent of the table construction under test.
template <typename T>
T Reference(std::span<const uint8_t> data, uint32_t poly, uint32_t init, bool reflected,
            uint32_t xorout) {
  constexpr uint32_t kWidth = sizeof(T) * 8U;
  const uint64_t mask = (uint64_t{1} << kWidth) - 1U;
  const uint64_t top = uint64_t{1} << (kWidth - 1U);
  uint64_t reg = init;
  for (const uint8_t byte : data) {
    uint32_t in = byte;
    if (reflected) {
      uint32_t r = 0U;
      for (uint32_t i = 0U; i < 8U; ++i) {
        r = (r << 1U) | ((in >> i) & 1U);
      }
      in = r;
    }
    reg ^= static_cast<uint64_t>(in) << (kWidth - 8U);
    for (uint32_t i = 0U; i < 8U; ++i) {
      reg = ((reg & top) != 0U) ? (((reg << 1U) ^ poly) & mask) : ((reg << 1U) & mask);
    }
  }
  if (reflected) {
    uint64_t r = 0U;
    for (uint32_t i = 0U; i < kWidth; ++i) {
      r = (r << 1U) | ((reg >> i) & 1U);
    }
    reg = r;
  }
  return static_cast<T>((reg ^ xorout) & mask);
}

template <typename CrcT, typename T>
void Check(std::span<const uint8_t> data, std::size_t split, uint32_t poly, uint32_t init,
           bool refl, uint32_t xorout) {
  const T expected = Reference<T>(data, poly, init, refl, xorout);
  if (CrcT::Compute(data) != expected) {
    std::abort();
  }
  CrcT crc{};
  crc.Update(data.first(split));
  crc.Update(data.subspan(split));
  if (crc.Value() != expected) {
    std::abort();
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  const std::span<const uint8_t> in{data, size};
  const std::size_t split = (size == 0U) ? 0U : (static_cast<std::size_t>(data[0]) % (size + 1U));
  Check<kb::utils::Crc8Smbus, uint8_t>(in, split, 0x07U, 0x00U, false, 0x00U);
  Check<kb::utils::Crc8Maxim, uint8_t>(in, split, 0x31U, 0x00U, true, 0x00U);
  Check<kb::utils::Crc8SaeJ1850, uint8_t>(in, split, 0x1DU, 0xFFU, false, 0xFFU);
  Check<kb::utils::Crc16CcittFalse, uint16_t>(in, split, 0x1021U, 0xFFFFU, false, 0U);
  Check<kb::utils::Crc16Modbus, uint16_t>(in, split, 0x8005U, 0xFFFFU, true, 0U);
  Check<kb::utils::Crc32Ieee, uint32_t>(in, split, 0x04C11DB7UL, 0xFFFFFFFFUL, true, 0xFFFFFFFFUL);
  Check<kb::utils::Crc32Mpeg2, uint32_t>(in, split, 0x04C11DB7UL, 0xFFFFFFFFUL, false, 0U);
  return 0;
}
