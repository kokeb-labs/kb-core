/**
 * @file crc.hpp
 * @brief Table-driven, constexpr CRC-8, CRC-16 and CRC-32 in the Rocksoft parameter model.
 *
 * `Crc<T, Poly, Init, Reflected, XorOut>` is one CRC: width from T (uint8_t, uint16_t or uint32_t),
 * the normal (MSB-first) polynomial, the initial register, whether input and output are reflected
 * (both or neither) and the final XOR. The 256-entry table is computed at compile time. Named
 * presets below carry their catalogue check value (the CRC of "123456789"), which the tests assert.
 *
 * @code
 * const std::array<uint8_t, 2> word{0xBEU, 0xEFU};
 * const uint8_t crc = kb::utils::Crc8Sensirion::Compute(word);   // 0x92
 * kb::utils::Crc32Ieee running{};
 * running.Update(first_half);
 * running.Update(second_half);
 * const uint32_t crc32 = running.Value();
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_CRC_HPP
#define KB_UTILS_CRC_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace kb::utils {

/** @brief A CRC register type: uint8_t, uint16_t or uint32_t. */
template <typename T>
concept CrcRegister =
    std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t>;

namespace detail {

/** @brief `value` with its lowest `bits` bits reversed (the rest cleared). */
template <CrcRegister T>
[[nodiscard]] constexpr T ReflectBits(T value, uint32_t bits) noexcept {
  auto in = static_cast<uint32_t>(value);
  uint32_t out = 0U;
  for (uint32_t i = 0U; i < bits; ++i) {
    out = (out << 1U) | (in & 1U);
    in >>= 1U;
  }
  return static_cast<T>(out);
}

/** @brief The register width of T in bits. */
template <CrcRegister T>
[[nodiscard]] constexpr uint32_t WidthOf() noexcept {
  return static_cast<uint32_t>(std::numeric_limits<T>::digits);
}

/** @brief The all-ones mask of T, as uint32_t. */
template <CrcRegister T>
[[nodiscard]] constexpr uint32_t MaskOf() noexcept {
  return static_cast<uint32_t>(std::numeric_limits<T>::max());
}

/** @brief The 256-entry table of a CRC; reflected tables use the reflected polynomial. */
template <CrcRegister T, T Poly, bool Reflected>
[[nodiscard]] constexpr std::array<T, 256> MakeCrcTable() noexcept {
  constexpr auto kWidth = static_cast<uint32_t>(std::numeric_limits<T>::digits);
  constexpr uint32_t kTop = 1UL << (kWidth - 1U);
  constexpr auto kMask = static_cast<uint32_t>(std::numeric_limits<T>::max());
  constexpr auto kPoly = static_cast<uint32_t>(Poly);
  constexpr auto kReflectedPoly = static_cast<uint32_t>(ReflectBits<T>(Poly, kWidth));
  std::array<T, 256> table{};
  for (uint32_t byte = 0U; byte < 256U; ++byte) {
    uint32_t reg = 0U;
    if constexpr (Reflected) {
      reg = byte;
      for (uint32_t bit = 0U; bit < 8U; ++bit) {
        reg = ((reg & 1U) != 0U) ? ((reg >> 1U) ^ kReflectedPoly) : (reg >> 1U);
      }
    } else {
      reg = byte << (kWidth - 8U);
      for (uint32_t bit = 0U; bit < 8U; ++bit) {
        reg = ((reg & kTop) != 0U) ? (((reg << 1U) ^ kPoly) & kMask) : ((reg << 1U) & kMask);
      }
    }
    table[byte] = static_cast<T>(reg);
  }
  return table;
}

} // namespace detail

/**
 * @brief One CRC algorithm, table-driven and constexpr.
 * @tparam T Register type (width 8, 16 or 32).
 * @tparam Poly Normal (MSB-first) polynomial without the top bit.
 * @tparam Init Initial register value, as the catalogue gives it.
 * @tparam Reflected Input and output reflected (RefIn = RefOut = true) or neither.
 * @tparam XorOut Value XORed into the register at the end.
 */
template <CrcRegister T, T Poly, T Init, bool Reflected, T XorOut>
class Crc {
public:
  /** @brief A fresh computation. */
  constexpr Crc() noexcept = default;

  /** @brief Feeds `data`; may be called any number of times. */
  constexpr void Update(std::span<const uint8_t> data) noexcept {
    for (const uint8_t byte : data) {
      reg_ = step(reg_, byte);
    }
  }

  /** @brief The CRC of everything fed since construction or the last Reset(). */
  [[nodiscard]] constexpr T Value() const noexcept {
    return static_cast<T>(reg_ ^ static_cast<uint32_t>(XorOut));
  }

  /** @brief Starts a new computation. */
  constexpr void Reset() noexcept {
    reg_ = kStart;
  }

  /** @brief The CRC of `data` in one call. */
  [[nodiscard]] static constexpr T Compute(std::span<const uint8_t> data) noexcept {
    Crc crc{};
    crc.Update(data);
    return crc.Value();
  }

private:
  static constexpr uint32_t kWidth = detail::WidthOf<T>();
  static constexpr uint32_t kMask = detail::MaskOf<T>();
  static constexpr std::array<T, 256> kTable = detail::MakeCrcTable<T, Poly, Reflected>();
  static constexpr uint32_t kStart =
      Reflected ? static_cast<uint32_t>(detail::ReflectBits<T>(Init, kWidth))
                : static_cast<uint32_t>(Init);

  // One table step. For an 8-bit register the shifted-out part is zero, so the same form serves
  // every width.
  [[nodiscard]] static constexpr uint32_t step(uint32_t reg, uint8_t byte) noexcept {
    const auto in = static_cast<uint32_t>(byte);
    uint32_t next = 0U;
    if constexpr (Reflected) {
      const uint32_t index = (reg ^ in) & 0xFFU;
      next = (reg >> 8U) ^ static_cast<uint32_t>(kTable[index]);
    } else {
      const uint32_t index = ((reg >> (kWidth - 8U)) ^ in) & 0xFFU;
      next = ((reg << 8U) & kMask) ^ static_cast<uint32_t>(kTable[index]);
    }
    return next;
  }

  uint32_t reg_{kStart};
};

/// CRC-8/SMBUS: poly 0x07, init 0x00, check 0xF4 (SMBus PEC).
using Crc8Smbus = Crc<uint8_t, 0x07U, 0x00U, false, 0x00U>;
/// CRC-8/NRSC-5 parameters as used by Sensirion sensors: poly 0x31, init 0xFF, check 0xF7.
using Crc8Sensirion = Crc<uint8_t, 0x31U, 0xFFU, false, 0x00U>;
/// CRC-8/MAXIM-DOW (1-Wire): poly 0x31 reflected, init 0x00, check 0xA1.
using Crc8Maxim = Crc<uint8_t, 0x31U, 0x00U, true, 0x00U>;
/// CRC-8/SAE-J1850: poly 0x1D, init 0xFF, xorout 0xFF, check 0x4B.
using Crc8SaeJ1850 = Crc<uint8_t, 0x1DU, 0xFFU, false, 0xFFU>;
/// CRC-16/IBM-3740 ("CCITT-FALSE"): poly 0x1021, init 0xFFFF, check 0x29B1.
using Crc16CcittFalse = Crc<uint16_t, 0x1021U, 0xFFFFU, false, 0x0000U>;
/// CRC-16/XMODEM: poly 0x1021, init 0x0000, check 0x31C3.
using Crc16Xmodem = Crc<uint16_t, 0x1021U, 0x0000U, false, 0x0000U>;
/// CRC-16/KERMIT: poly 0x1021 reflected, init 0x0000, check 0x2189.
using Crc16Kermit = Crc<uint16_t, 0x1021U, 0x0000U, true, 0x0000U>;
/// CRC-16/MODBUS: poly 0x8005 reflected, init 0xFFFF, check 0x4B37.
using Crc16Modbus = Crc<uint16_t, 0x8005U, 0xFFFFU, true, 0x0000U>;
/// CRC-32/ISO-HDLC (Ethernet, zlib): poly 0x04C11DB7 reflected, init and xorout 0xFFFFFFFF, check
/// 0xCBF43926.
using Crc32Ieee = Crc<uint32_t, 0x04C11DB7UL, 0xFFFFFFFFUL, true, 0xFFFFFFFFUL>;
/// CRC-32/ISCSI (Castagnoli): poly 0x1EDC6F41 reflected, init and xorout 0xFFFFFFFF, check
/// 0xE3069283.
using Crc32C = Crc<uint32_t, 0x1EDC6F41UL, 0xFFFFFFFFUL, true, 0xFFFFFFFFUL>;
/// CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, xorout 0, check 0x0376E6E7.
using Crc32Mpeg2 = Crc<uint32_t, 0x04C11DB7UL, 0xFFFFFFFFUL, false, 0x00000000UL>;

} // namespace kb::utils

#endif // KB_UTILS_CRC_HPP
