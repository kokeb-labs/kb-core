// Tests of the durations, instants and wrap-safe elapsed time (time.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/utils/time.hpp"
#include "kb_test.hpp"

using kb::Instant;
using kb::Microseconds;
using kb::Milliseconds;

KB_TEST("time: durations compare and keep their count") {
  KB_CHECK_EQ(Milliseconds{7U}.Count(), 7U);
  KB_CHECK_EQ(Microseconds{}.Count(), 0U);
  KB_CHECK(Milliseconds{1U} < Milliseconds{2U});
  KB_CHECK(Milliseconds{2U} == Milliseconds{2U});
  KB_CHECK(Milliseconds{3U} != Milliseconds{2U});
  KB_CHECK(Microseconds{1U} < Microseconds{2U});
  KB_CHECK(Microseconds{2U} == Microseconds{2U});
  KB_CHECK(Microseconds{9U} >= Microseconds{2U});
  KB_CHECK(Instant{5U} == Instant{5U});
  KB_CHECK(!(Instant{5U} == Instant{6U}));
  KB_CHECK_EQ(kb::kMaxMilliseconds.Count(), UINT32_MAX);
}

KB_TEST("time: conversions round in the safe direction") {
  KB_CHECK_EQ(kb::utils::ToMicroseconds(Milliseconds{kb_test::Rt(3U)}).Count(), kb_test::Rt(3000U));
  KB_CHECK_EQ(kb::utils::ToMicroseconds(Milliseconds{kb_test::Rt(4294967U)}).Count(),
              kb_test::Rt(4294967000U));
  KB_CHECK_EQ(kb::utils::ToMicroseconds(Milliseconds{kb_test::Rt(4294968U)}).Count(),
              kb_test::Rt(UINT32_MAX));
  KB_CHECK_EQ(kb::utils::ToMillisecondsCeil(Microseconds{kb_test::Rt(0U)}).Count(),
              kb_test::Rt(0U));
  KB_CHECK_EQ(kb::utils::ToMillisecondsCeil(Microseconds{kb_test::Rt(1U)}).Count(),
              kb_test::Rt(1U));
  KB_CHECK_EQ(kb::utils::ToMillisecondsCeil(Microseconds{kb_test::Rt(1000U)}).Count(),
              kb_test::Rt(1U));
  KB_CHECK_EQ(kb::utils::ToMillisecondsCeil(Microseconds{kb_test::Rt(1001U)}).Count(),
              kb_test::Rt(2U));
  KB_CHECK_EQ(kb::utils::ToMillisecondsCeil(Microseconds{kb_test::Rt(UINT32_MAX)}).Count(),
              kb_test::Rt(4294968U));
}

KB_TEST("time: milliseconds to ticks round up and saturate") {
  KB_CHECK_EQ(
      kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(10U)}, kb_test::Rt(1000U)),
      kb_test::Rt(10U));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(10U)}, kb_test::Rt(100U)),
              kb_test::Rt(1U));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(11U)}, kb_test::Rt(100U)),
              kb_test::Rt(2U));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(1U)}, kb_test::Rt(100U)),
              kb_test::Rt(1U));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(0U)}, kb_test::Rt(100U)),
              kb_test::Rt(0U));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(Milliseconds{kb_test::Rt(5U)}, kb_test::Rt(0U)),
              kb_test::Rt(UINT32_MAX));
  KB_CHECK_EQ(kb::utils::MillisecondsToTicksCeil(kb::kMaxMilliseconds, kb_test::Rt(100000U)),
              kb_test::Rt(UINT32_MAX));
}

KB_TEST("time: elapsed time is right across a wrap of the counter") {
  KB_CHECK_EQ(kb::utils::ElapsedSince(kb_test::Rt(5U), kb_test::Rt(0xFFFFFFFBU)), kb_test::Rt(10U));
  KB_CHECK_EQ(kb::utils::ElapsedSince(kb_test::Rt(100U), kb_test::Rt(40U)), kb_test::Rt(60U));
  KB_CHECK(kb::utils::HasElapsed(kb_test::Rt(5U), kb_test::Rt(0xFFFFFFFBU), kb_test::Rt(10U)));
  KB_CHECK(!kb::utils::HasElapsed(kb_test::Rt(4U), kb_test::Rt(0xFFFFFFFBU), kb_test::Rt(10U)));
  // 2^31 apart: a signed half-range compare would get this wrong, the unsigned difference does not.
  KB_CHECK(
      kb::utils::HasElapsed(kb_test::Rt(0x80000001U), kb_test::Rt(1U), kb_test::Rt(0x80000000U)));
  KB_CHECK_EQ(kb::utils::ElapsedUs(Instant{kb_test::Rt(3U)},
                                   Instant{kb_test::Rt(UINT64_MAX) - kb_test::Rt(1U)}),
              kb_test::Rt(5U));
}

KB_TEST("time: narrowing keeps the low 32 bits") {
  const Instant t{0x1'0000'0005ULL};
  KB_CHECK_EQ(kb::utils::NarrowUs(t), kb_test::Rt(5U));
  KB_CHECK_EQ(kb::utils::NarrowMs(Instant{kb_test::Rt(5'000'123ULL)}), kb_test::Rt(5000U));
  KB_CHECK_EQ(kb::utils::NarrowMs(Instant{kb_test::Rt(UINT64_MAX)}),
              static_cast<uint32_t>((UINT64_MAX / 1000U) & 0xFFFFFFFFU));
}

KB_TEST("time: deadlines expire by elapsed time, also across a wrap") {
  const kb::utils::Deadline d = kb::utils::Deadline::After(
      Instant{kb_test::Rt(UINT64_MAX) - kb_test::Rt(5U)}, Microseconds{kb_test::Rt(10U)});
  KB_CHECK(!d.Expired(Instant{UINT64_MAX - 5U}));
  KB_CHECK_EQ(d.RemainingUs(Instant{UINT64_MAX}), 5U);
  KB_CHECK(!d.Expired(Instant{3U}));
  KB_CHECK(d.Expired(Instant{4U}));
  KB_CHECK_EQ(d.RemainingUs(Instant{100U}), 0U);
  const kb::utils::Deadline m =
      kb::utils::Deadline::After(Instant{kb_test::Rt(1000U)}, Milliseconds{kb_test::Rt(2U)});
  KB_CHECK(!m.Expired(Instant{2999U}));
  KB_CHECK(m.Expired(Instant{3000U}));
  const kb::utils::Deadline none{};
  KB_CHECK(none.Expired(Instant{}));
}

static_assert(kb::utils::ToMillisecondsCeil(Microseconds{1500U}) == Milliseconds{2U});
