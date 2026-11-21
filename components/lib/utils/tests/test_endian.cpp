// Tests of the byte-order loads and stores and the bounded readers and writers (endian.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <span>

#include "kb/utils/endian.hpp"
#include "kb_test.hpp"

namespace u = kb::utils;

KB_TEST("endian: loads read the documented byte order") {
  const std::array<uint8_t, 8> b{0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
  const std::span<const uint8_t> s{b};
  KB_CHECK_EQ(u::LoadBe16(s.first<2>()), 0x0102U);
  KB_CHECK_EQ(u::LoadLe16(s.first<2>()), 0x0201U);
  KB_CHECK_EQ(u::LoadBe32(s.first<4>()), 0x01020304UL);
  KB_CHECK_EQ(u::LoadLe32(s.first<4>()), 0x04030201UL);
  KB_CHECK(u::LoadBe64(s.first<8>()) == 0x0102030405060708ULL);
  KB_CHECK(u::LoadLe64(s.first<8>()) == 0x0807060504030201ULL);
  KB_CHECK_EQ(u::AsSigned16(0xFFFEU), -2);
  KB_CHECK_EQ(u::AsSigned32(0x80000000UL), INT32_MIN);
}

KB_TEST("endian: stores round-trip with loads") {
  std::array<uint8_t, 8> b{};
  const std::span<uint8_t> s{b};
  u::StoreBe16(0xA1B2U, s.first<2>());
  KB_CHECK_EQ(b[0], 0xA1U);
  KB_CHECK_EQ(u::LoadBe16(std::span<const uint8_t, 2>{s.first<2>()}), 0xA1B2U);
  u::StoreLe16(0xA1B2U, s.first<2>());
  KB_CHECK_EQ(b[0], 0xB2U);
  u::StoreBe32(0xDEADBEEFUL, s.first<4>());
  KB_CHECK_EQ(b[0], 0xDEU);
  KB_CHECK_EQ(b[3], 0xEFU);
  u::StoreLe32(0xDEADBEEFUL, s.first<4>());
  KB_CHECK_EQ(b[0], 0xEFU);
  KB_CHECK_EQ(b[3], 0xDEU);
  u::StoreBe64(0x1122334455667788ULL, s.first<8>());
  KB_CHECK_EQ(b[0], 0x11U);
  KB_CHECK_EQ(b[7], 0x88U);
  u::StoreLe64(0x1122334455667788ULL, s.first<8>());
  KB_CHECK_EQ(b[0], 0x88U);
  KB_CHECK_EQ(b[7], 0x11U);
}

KB_TEST("endian: a reader parses a frame and stops sticky at the first overrun") {
  const std::array<uint8_t, 19> frame{0x7EU, 0x12U, 0x34U, 0x34U, 0x12U, 0x01U, 0x02U,
                                      0x03U, 0x04U, 0x04U, 0x03U, 0x02U, 0x01U, 0xAAU,
                                      0xBBU, 0xCCU, 0x00U, 0x00U, 0x09U};
  u::ByteReader r{frame};
  uint8_t id = 0U;
  uint16_t be16 = 0U;
  uint16_t le16 = 0U;
  uint32_t be32 = 0U;
  uint32_t le32 = 0U;
  std::array<uint8_t, 2> raw{};
  KB_CHECK(r.ReadU8(id) && r.ReadBe16(be16) && r.ReadLe16(le16) && r.ReadBe32(be32) &&
           r.ReadLe32(le32));
  KB_CHECK_EQ(id, 0x7EU);
  KB_CHECK_EQ(be16, 0x1234U);
  KB_CHECK_EQ(le16, 0x1234U);
  KB_CHECK_EQ(be32, 0x01020304UL);
  KB_CHECK_EQ(le32, 0x01020304UL);
  KB_CHECK(!r.AtEnd());
  KB_CHECK(r.ReadBytes(raw) && (raw[1] == 0xBBU));
  KB_CHECK(r.Skip(1U));
  KB_CHECK_EQ(r.Position(), 16U);
  KB_CHECK_EQ(r.Remaining(), 3U);
  uint32_t too_long = 0U;
  KB_CHECK(!r.ReadBe32(too_long));
  KB_CHECK(!r.Ok());
  KB_CHECK_EQ(r.Remaining(), 0U);
  KB_CHECK(!r.ReadU8(id));
  KB_CHECK(!r.AtEnd());
}

KB_TEST("endian: a reader reads 64-bit fields and knows when it is at the end") {
  const std::array<uint8_t, 16> b{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
  u::ByteReader r{b};
  uint64_t le = 0U;
  uint64_t be = 0U;
  KB_CHECK(r.ReadLe64(le) && r.ReadBe64(be));
  KB_CHECK(le == 1U);
  KB_CHECK(be == 1U);
  KB_CHECK(r.AtEnd());
  uint16_t x = 0U;
  KB_CHECK(!r.ReadLe16(x));
  u::ByteReader r2{b};
  uint32_t y = 0U;
  KB_CHECK(r2.Skip(14U) && !r2.ReadLe32(y) && !r2.ReadBe16(x) && !r2.Skip(0U));
  uint64_t z = 0U;
  KB_CHECK(!r2.ReadBe64(z) && !r2.ReadLe64(z));
  std::array<uint8_t, 4> quad{0x42U, 0U, 0U, 0U};
  u::ByteReader r3{std::span<const uint8_t>{quad.data(), kb_test::Rt(std::size_t{1})}};
  std::array<uint8_t, 2> two{};
  KB_CHECK(!r3.ReadBytes(two));
}

KB_TEST("endian: a writer builds a frame and refuses what does not fit") {
  std::array<uint8_t, 32> buffer{};
  u::ByteWriter w{buffer};
  const std::array<uint8_t, 2> tail{0xEEU, 0xFFU};
  KB_CHECK(w.WriteU8(0x7EU) && w.WriteBe16(0x1234U) && w.WriteLe16(0x1234U) && w.WriteBe32(1U) &&
           w.WriteLe32(1U) && w.WriteBe64(2U) && w.WriteLe64(2U) && w.WriteBytes(tail));
  KB_CHECK_EQ(w.Written().size(), 31U);
  KB_CHECK_EQ(buffer[1], 0x12U);
  KB_CHECK_EQ(buffer[3], 0x34U);
  KB_CHECK_EQ(buffer[30], 0xFFU);
  KB_CHECK_EQ(w.Remaining(), 1U);
  KB_CHECK(!w.WriteBe16(0U));
  KB_CHECK(!w.Ok());
  KB_CHECK_EQ(w.Remaining(), 0U);
  KB_CHECK(!w.WriteU8(0U));
  KB_CHECK_EQ(w.Written().size(), 31U);
  std::array<uint8_t, 3> small{};
  u::ByteWriter s{small};
  KB_CHECK(!s.WriteLe32(0U) && !s.WriteBe32(0U) && !s.WriteBe64(0U) && !s.WriteLe64(0U) &&
           !s.WriteLe16(0U) && !s.WriteBytes(tail));
}
