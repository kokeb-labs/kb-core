// The platform-free backend rules (kb/rtos/backend_rules.hpp): delay-until arithmetic across a
// tick wrap, the late wake-up check, the holder's retake sleep and the 64-bit tick extension.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "kb/rtos/backend_rules.hpp"
#include "kb_test.hpp"

namespace d = kb::rtos::detail;

KB_TEST("backend rules: delay-until sleeps the rest of the period or reports the overrun") {
  const d::DelayUntilPlan early = d::PlanDelayUntil(104U, 100U, 10U);
  KB_CHECK(early.on_time);
  KB_CHECK_EQ(early.sleep_ticks, 6U);
  const d::DelayUntilPlan exact = d::PlanDelayUntil(110U, 100U, 10U);
  KB_CHECK(!exact.on_time);
  KB_CHECK_EQ(exact.sleep_ticks, 0U);
  const d::DelayUntilPlan late = d::PlanDelayUntil(125U, 100U, 10U);
  KB_CHECK(!late.on_time);
  KB_CHECK_EQ(late.sleep_ticks, 0U);
  const d::DelayUntilPlan wrap = d::PlanDelayUntil(2U, 0xFFFFFFFCUL, 10U);
  KB_CHECK(wrap.on_time);
  KB_CHECK_EQ(wrap.sleep_ticks, 4U);
  KB_CHECK_EQ(d::PlanDelayUntil(100U, 100U, 1U).sleep_ticks, 1U);
}

KB_TEST("backend rules: a wake-up after the grid point is late") {
  KB_CHECK(d::WokeOnTime(110U, 100U, 10U));
  KB_CHECK(d::WokeOnTime(109U, 100U, 10U));
  KB_CHECK(!d::WokeOnTime(111U, 100U, 10U));
  KB_CHECK(d::WokeOnTime(4U, 0xFFFFFFFCUL, 10U));
  KB_CHECK(!d::WokeOnTime(7U, 0xFFFFFFFCUL, 10U));
}

KB_TEST("backend rules: the holder's retake waits its timeout, never forever") {
  KB_CHECK_EQ(d::HolderRetakeSleep(0U), 0U);
  KB_CHECK_EQ(d::HolderRetakeSleep(7U), 7U);
  KB_CHECK_EQ(d::HolderRetakeSleep(UINT32_MAX - 1U), UINT32_MAX - 1U);
  KB_CHECK_EQ(d::HolderRetakeSleep(UINT32_MAX), 0U);
}

KB_TEST("backend rules: the tick extension counts wraps, not equal or rising readings") {
  d::TickExtender fresh{};
  KB_CHECK_EQ(fresh.Extend(0U), 0U); // a first reading of zero is not a wrap
  d::TickExtender ext{};
  KB_CHECK_EQ(ext.Extend(5U), 5U);
  KB_CHECK_EQ(ext.Extend(5U), 5U);
  KB_CHECK_EQ(ext.Extend(0xFFFFFFFFUL), 0xFFFFFFFFULL);
  KB_CHECK_EQ(ext.Extend(3U), 0x100000003ULL);
  KB_CHECK_EQ(ext.Extend(3U), 0x100000003ULL);
  KB_CHECK_EQ(ext.Extend(1U), 0x200000001ULL);
}

namespace {
// A scripted clock: each reading returns the next value (the last repeats); naps are counted.
template <std::size_t N>
struct Script {
  std::array<uint64_t, N> readings;
  uint32_t next{0U};
  uint32_t naps{0U};
  uint64_t Read() noexcept {
    const std::size_t i = (next < N) ? next : (N - 1U);
    ++next;
    return readings.at(i);
  }
};

template <std::size_t N>
bool Poll(Script<N>& s, bool condition, uint64_t limit_us) noexcept {
  return d::PollUntil([&s]() noexcept { return s.Read(); }, [&s]() noexcept { ++s.naps; },
                      [condition]() noexcept { return condition; }, limit_us);
}
} // namespace

KB_TEST("backend rules: a clock that steps back never ends a poll early") {
  // 0, 400 ms, back to 100 ms, then forward 150 ms each: 400 + 0 + 150 + 150 + 150 >= 800.
  Script<7> s{.readings = {0U, 400000U, 100000U, 250000U, 400000U, 550000U, 700000U}};
  KB_CHECK(!Poll(s, false, 800000U));
  // Reading 0 is the start; the poll ends once forward steps sum to 800 ms: 400+0+150+150 = 700
  // (4 naps), +150 = 850 (5 naps). Counting the backward step as time would have ended at 3 naps.
  KB_CHECK_EQ(s.naps, 5U);
}

KB_TEST("backend rules: a 64-bit clock wrap counts as forward time") {
  constexpr uint64_t kTop = 0xFFFFFFFFFFFFFF00ULL;
  KB_CHECK_EQ(d::ForwardUs(0x20U, kTop), 0x120U);
  KB_CHECK_EQ(d::ForwardUs(5U, 9U), 0U);
  // Exactly half the range is a step back, one less is still forward.
  constexpr uint64_t kHalf = uint64_t{1U} << 63U;
  KB_CHECK_EQ(d::ForwardUs(kHalf, 0U), 0U);
  KB_CHECK_EQ(d::ForwardUs(kHalf - 1U, 0U), kHalf - 1U);
  Script<4> s{
      .readings = {kTop, 0xFFFFFFFFFFFFFFF0ULL, 0x00000000000000E0ULL, 0x0000000000000200ULL}};
  // 240 + 240 + 288 us of forward time; a 500 us limit is reached on the third nap.
  KB_CHECK(!Poll(s, false, 500U));
  KB_CHECK_EQ(s.naps, 3U);
}

KB_TEST("backend rules: a poll that is done at once never naps") {
  Script<1> s{.readings = {0U}};
  KB_CHECK(Poll(s, true, 1000U));
  KB_CHECK_EQ(s.naps, 0U);
}

KB_TEST("backend rules: a poll with nothing spent yet naps before it tests the limit") {
  // One microsecond of forward time ends it: the first nap is always taken, a second never is.
  Script<2> s{.readings = {0U, 1U}};
  KB_CHECK(!Poll(s, false, 1U));
  KB_CHECK_EQ(s.naps, 1U);
}
