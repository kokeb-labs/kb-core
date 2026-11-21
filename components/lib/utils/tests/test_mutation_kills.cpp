// Tests that pin behaviour at the exact edges the mutation run probes: enumerator values,
// arithmetic boundaries, first and last elements of every copy, exact-fit writes and default
// states. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "kb/utils/utils.hpp"
#include "kb_test.hpp"

using kb_test::Rt;
namespace u = kb::utils;

KB_TEST("status values are the error-code contract: each enumerator keeps its number") {
  constexpr std::array<kb::Status, kb::kStatusCount> kAll{
      kb::Status::Ok,          kb::Status::InvalidArgument,
      kb::Status::OutOfRange,  kb::Status::Timeout,
      kb::Status::Busy,        kb::Status::NotReady,
      kb::Status::NoDevice,    kb::Status::Nack,
      kb::Status::BusError,    kb::Status::Overflow,
      kb::Status::Full,        kb::Status::Empty,
      kb::Status::Unsupported, kb::Status::AlreadyStarted,
      kb::Status::CrcMismatch, kb::Status::Fault};
  for (std::size_t i = 0U; i < kAll.size(); ++i) {
    KB_CHECK_EQ(static_cast<std::size_t>(kAll[i]), i);
  }
  KB_CHECK(std::string_view{kb::ToString(Rt(static_cast<kb::Status>(15U)))} == "Fault");
  // The first value past the name table must give "Unknown", never index the table.
  const auto past = static_cast<kb::Status>(kb::kStatusCount);
  KB_CHECK(std::string_view{kb::ToString(Rt(past))} == "Unknown");
}

KB_TEST("checked add and sub are exact at the 64-bit limits") {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr uint64_t kUMax = std::numeric_limits<uint64_t>::max();
  int64_t s = 0;
  uint64_t us = 0U;
  KB_CHECK(u::CheckedAdd(Rt(kMax - 1), Rt(int64_t{1}), s) && (s == kMax));
  KB_CHECK(u::CheckedAdd(Rt(kMin + 1), Rt(int64_t{-1}), s) && (s == kMin));
  KB_CHECK(!u::CheckedAdd(Rt(kMin), Rt(int64_t{-1}), s));
  KB_CHECK(u::CheckedAdd(Rt(kMin), Rt(int64_t{0}), s) && (s == kMin));
  KB_CHECK(u::CheckedAdd(Rt(kUMax - 1U), Rt(uint64_t{1}), us) && (us == kUMax));
  KB_CHECK(u::CheckedSub(Rt(kMax - 1), Rt(int64_t{-1}), s) && (s == kMax));
  KB_CHECK(!u::CheckedSub(Rt(kMax), Rt(int64_t{-1}), s));
  KB_CHECK(u::CheckedSub(Rt(kMin + 1), Rt(int64_t{1}), s) && (s == kMin));
  KB_CHECK(!u::CheckedSub(Rt(kMin), Rt(int64_t{1}), s));
  KB_CHECK(u::CheckedSub(Rt(uint64_t{5}), Rt(uint64_t{5}), us) && (us == 0U));
  KB_CHECK(u::CheckedSub(Rt(kMax), Rt(int64_t{0}), s) && (s == kMax));
}

KB_TEST("checked mul is exact at the 64-bit limits, in every sign combination") {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr uint64_t kUMax = std::numeric_limits<uint64_t>::max();
  int64_t s = 0;
  uint64_t us = 0U;
  KB_CHECK(u::CheckedMul(Rt(int64_t{1}), Rt(int64_t{5}), s) && (s == 5));
  KB_CHECK(u::CheckedMul(Rt(int64_t{1}), Rt(int64_t{-5}), s) && (s == -5));
  KB_CHECK(u::CheckedMul(Rt(int64_t{5}), Rt(int64_t{1}), s) && (s == 5));
  KB_CHECK(u::CheckedMul(Rt(uint64_t{1}), Rt(uint64_t{7}), us) && (us == 7U));
  KB_CHECK(u::CheckedMul(Rt(kUMax / 3U), Rt(uint64_t{3}), us) && (us == kUMax));
  KB_CHECK(!u::CheckedMul(Rt((kUMax / 3U) + 1U), Rt(uint64_t{3}), us));
  KB_CHECK(u::CheckedMul(Rt(kMax), Rt(int64_t{1}), s) && (s == kMax));
  KB_CHECK(u::CheckedMul(Rt(int64_t{1}), Rt(kMax), s) && (s == kMax));
  KB_CHECK(u::CheckedMul(Rt(kMin / 2), Rt(int64_t{2}), s) && (s == kMin));
  KB_CHECK(u::CheckedMul(Rt(int64_t{2}), Rt(kMin / 2), s) && (s == kMin));
  KB_CHECK(!u::CheckedMul(Rt((kMin / 2) - 1), Rt(int64_t{2}), s));
  KB_CHECK(u::CheckedMul(Rt(kMin / -2), Rt(int64_t{-2}), s) && (s == kMin));
  KB_CHECK(u::CheckedMul(Rt(int64_t{-2}), Rt(kMin / -2), s) && (s == kMin));
  KB_CHECK(!u::CheckedMul(Rt(int64_t{-2}), Rt((kMin / -2) + 1), s));
  KB_CHECK(u::CheckedMul(Rt(int64_t{-1}), Rt(-kMax), s) && (s == kMax));
  KB_CHECK(u::CheckedMul(Rt(-kMax), Rt(int64_t{-1}), s) && (s == kMax));
  KB_CHECK(u::CheckedMul(Rt(int64_t{-3}), Rt(kMax / -3), s) && (s == kMax - 1));
}

KB_TEST("time conversions and the clock defaults") {
  KB_CHECK_EQ(kb::Milliseconds{}.Count(), 0U);
  KB_CHECK_EQ(kb::Instant{}.Us(), 0U);
  KB_CHECK_EQ(u::MillisecondsToTicksCeil(kb::Milliseconds{Rt(1001U)}, Rt(1000U)), 1001U);
  KB_CHECK_EQ(u::MillisecondsToTicksCeil(kb::Milliseconds{Rt(1U)}, Rt(1000000U)), 1000U);
}

KB_TEST("spans: equal sizes copy, the first element is compared and copied") {
  std::array<uint8_t, 2> dst{};
  std::array<uint8_t, 2> src{7U, 8U};
  KB_CHECK(u::CopyInto<uint8_t>(dst, std::span<const uint8_t>{src.data(), Rt(src.size())}));
  KB_CHECK((dst[0] == 7U) && (dst[1] == 8U));
  std::array<uint8_t, 2> other{6U, 8U};
  KB_CHECK(!u::Equal<uint8_t>(std::span<const uint8_t>{dst}, std::span<const uint8_t>{other}));
  KB_CHECK(!u::ConstantTimeEqual(dst, other));
  u::Fill<uint8_t>(dst, Rt(uint8_t{9U}));
  KB_CHECK((dst[0] == 9U) && (dst[1] == 9U));
}

KB_TEST("fixed string: zero, defaults, and the terminator after Clear") {
  u::FixedString<8> s{};
  KB_CHECK(!s.Truncated());
  KB_CHECK(s.AppendSigned(Rt(int64_t{0})) && (s.View() == "0"));
  KB_CHECK(s.Append("ab"));
  s.Clear();
  KB_CHECK_EQ(s.CStr()[0], '\0');
  KB_CHECK(s.AppendHex(Rt(uint64_t{0xABU})) && (s.View() == "AB"));
}

KB_TEST("results report no detail unless given one") {
  KB_CHECK_EQ(kb::Result<int>::Failure(Rt(kb::Status::Busy)).Detail(), 0U);
  KB_CHECK_EQ(kb::Result<void>::Failure(Rt(kb::Status::Busy)).Detail(), 0U);
}

KB_TEST("static vector: erase shifts everything after it; clear and regrow give value-initialised "
        "items") {
  u::StaticVector<uint8_t, 4> v{};
  KB_CHECK(v.PushBack(1U) && v.PushBack(2U) && v.PushBack(3U) && v.PushBack(4U));
  KB_CHECK(v.Erase(Rt(std::size_t{1})));
  uint8_t x = 0U;
  KB_CHECK(v.At(0U, x) && (x == 1U));
  KB_CHECK(v.At(1U, x) && (x == 3U));
  KB_CHECK(v.At(2U, x) && (x == 4U));
  KB_CHECK(v.PushBack(5U));
  v.Clear();
  KB_CHECK(v.Resize(Rt(std::size_t{4})));
  for (std::size_t i = 0U; i < 4U; ++i) {
    KB_CHECK(v.At(i, x) && (x == 0U));
  }
  u::StaticVector<uint8_t, 2> full{};
  KB_CHECK(full.PushBack(1U) && full.PushBack(2U) && full.Resize(Rt(std::size_t{1})));
  KB_CHECK_EQ(full.Size(), 1U);
  u::StaticVector<uint8_t, 1> one{};
  KB_CHECK(one.PushBack(7U) && one.PopBack(x) && (x == 7U) && one.Empty());
  u::RingBuffer<uint8_t, 1> ring{};
  KB_CHECK(ring.Push(3U) && ring.Pop(x) && (x == 3U));
}

KB_TEST("endian: every byte of a store, exact-fit writes and the first byte of a bulk copy") {
  std::array<uint8_t, 8> b{};
  u::StoreBe32(Rt(uint32_t{0x11223344U}), std::span<uint8_t>{b}.first<4>());
  KB_CHECK((b[0] == 0x11U) && (b[1] == 0x22U) && (b[2] == 0x33U) && (b[3] == 0x44U));
  u::StoreLe32(Rt(uint32_t{0x11223344U}), std::span<uint8_t>{b}.first<4>());
  KB_CHECK((b[0] == 0x44U) && (b[1] == 0x33U) && (b[2] == 0x22U) && (b[3] == 0x11U));
  u::StoreLe16(Rt(uint16_t{0xA1B2U}), std::span<uint8_t>{b}.first<2>());
  KB_CHECK((b[0] == 0xB2U) && (b[1] == 0xA1U));
  u::StoreBe16(Rt(uint16_t{0xA1B2U}), std::span<uint8_t>{b}.first<2>());
  KB_CHECK((b[0] == 0xA1U) && (b[1] == 0xB2U));
  u::StoreBe64(Rt(0x0102030405060708ULL), std::span<uint8_t>{b}.first<8>());
  for (std::size_t i = 0U; i < 8U; ++i) {
    KB_CHECK_EQ(b[i], i + 1U);
  }
  u::StoreLe64(Rt(0x0102030405060708ULL), std::span<uint8_t>{b}.first<8>());
  for (std::size_t i = 0U; i < 8U; ++i) {
    KB_CHECK_EQ(b[i], 8U - i);
  }
  std::array<uint8_t, 2> exact{};
  u::ByteWriter w{std::span<uint8_t>{exact.data(), Rt(exact.size())}};
  const std::array<uint8_t, 2> two{0xA1U, 0xB2U};
  KB_CHECK(w.WriteBytes(two) && (w.Remaining() == 0U));
  KB_CHECK((exact[0] == 0xA1U) && (exact[1] == 0xB2U));
  u::ByteReader r{std::span<const uint8_t>{exact.data(), Rt(exact.size())}};
  std::array<uint8_t, 2> back{};
  KB_CHECK(r.ReadBytes(back) && (back[0] == 0xA1U) && (back[1] == 0xB2U));
}
