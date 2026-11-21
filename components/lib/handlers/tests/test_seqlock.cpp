// kb::handlers::Seqlock<T> with one context: publish and read back values of every size modulo the
// 4-byte word, the publish count, and the Snapshot and Stamped defaults. Torn reads against a
// concurrent writer are in test_threads.cpp.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "kb/handlers/common/seqlock.hpp"
#include "kb_test.hpp"

using kb::Status;
using kb::handlers::Seqlock;
namespace h = kb::handlers;

namespace {

template <std::size_t N>
struct Bytes {
  std::array<uint8_t, N> b{};
};

// Publishes three patterns into a Seqlock<Bytes<N>> and reads each back exactly.
template <std::size_t N>
bool RoundTrip() noexcept {
  Seqlock<Bytes<N>> cell{};
  bool ok = cell.Publishes() == 0U;
  const kb::Result<Bytes<N>> initial = cell.Read();
  ok = ok && initial.IsOk();
  for (std::size_t i = 0U; i < N; ++i) {
    ok = ok && (initial.Value().b[i] == 0U);
  }
  for (uint32_t round = 1U; round <= 3U; ++round) {
    Bytes<N> v{};
    for (std::size_t i = 0U; i < N; ++i) {
      v.b[i] = static_cast<uint8_t>((round * 37U) + (static_cast<uint32_t>(i) * 11U) + 1U);
    }
    cell.Publish(v);
    Bytes<N> out{};
    uint32_t publishes = 0U;
    ok = ok && cell.TryRead(out, publishes) && (publishes == round) && (cell.Publishes() == round);
    const kb::Result<Bytes<N>> read = cell.Read();
    ok = ok && read.IsOk();
    for (std::size_t i = 0U; i < N; ++i) {
      ok = ok && (out.b[i] == v.b[i]) && (read.Value().b[i] == v.b[i]);
    }
  }
  return ok;
}

struct Mixed {
  uint64_t big{0U};
  uint16_t half{0U};
  uint8_t small{0U};
  bool flag{false};
  int32_t sign{0};
};

} // namespace

KB_TEST("seqlock: values of 1 to 13 bytes survive the word packing") {
  KB_CHECK(RoundTrip<1>());
  KB_CHECK(RoundTrip<2>());
  KB_CHECK(RoundTrip<3>());
  KB_CHECK(RoundTrip<4>());
  KB_CHECK(RoundTrip<5>());
  KB_CHECK(RoundTrip<6>());
  KB_CHECK(RoundTrip<7>());
  KB_CHECK(RoundTrip<8>());
  KB_CHECK(RoundTrip<13>());
}

KB_TEST("seqlock: a struct with mixed fields and signs reads back field by field") {
  Seqlock<Mixed> cell{};
  Mixed v{};
  v.big = 0x0123456789ABCDEFULL;
  v.half = 0xBEEFU;
  v.small = 0x7FU;
  v.flag = true;
  v.sign = -123456;
  cell.Publish(v);
  const kb::Result<Mixed> r = cell.Read();
  KB_REQUIRE(r.IsOk());
  KB_CHECK_EQ(r.Value().big, 0x0123456789ABCDEFULL);
  KB_CHECK_EQ(r.Value().half, 0xBEEFU);
  KB_CHECK_EQ(r.Value().small, 0x7FU);
  KB_CHECK(r.Value().flag);
  KB_CHECK_EQ(r.Value().sign, -123456);
}

KB_TEST("seqlock: the publish count grows by one per publish") {
  Seqlock<uint32_t> cell{};
  for (uint32_t i = 0U; i < 50U; ++i) {
    cell.Publish(i * 3U);
    KB_CHECK_EQ(cell.Publishes(), i + 1U);
    uint32_t out = 0U;
    uint32_t publishes = 0U;
    KB_REQUIRE(cell.TryRead(out, publishes));
    KB_CHECK_EQ(out, i * 3U);
    KB_CHECK_EQ(publishes, i + 1U);
  }
}

KB_TEST("seqlock: Snapshot and Stamped start empty") {
  const h::Snapshot<uint16_t> snap{};
  KB_CHECK_EQ(snap.value, 0U);
  KB_CHECK_EQ(snap.sequence, 0U);
  KB_CHECK_EQ(snap.age_us, 0U);
  const h::Stamped<uint16_t> stamped{};
  KB_CHECK(!stamped.valid);
  KB_CHECK_EQ(stamped.at_us, 0U);
  static_assert(h::SnapshotValue<Mixed>);
  static_assert(sizeof(h::ObjectBytes<Mixed>) == sizeof(Mixed));
  KB_CHECK_EQ(h::kReadAttempts, 4U);
}
