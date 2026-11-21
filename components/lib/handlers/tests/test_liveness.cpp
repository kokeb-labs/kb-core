// kb::handlers::LivenessBeat and LivenessMonitor: beats, watch slots and their refusals, stale
// bits, progress, parked beats and the watchdog verdict.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/handlers/common/liveness.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
using kb::handlers::LivenessBeat;
using kb::handlers::LivenessMonitor;

KB_TEST("liveness: a beat counts and remembers its instant") {
  LivenessBeat beat{};
  KB_CHECK_EQ(beat.Beats(), 0U);
  KB_CHECK_EQ(beat.LastBeat().Us(), 0U);
  beat.Beat(kb::Instant{1500U});
  beat.Beat(kb::Instant{2500U});
  KB_CHECK_EQ(beat.Beats(), 2U);
  KB_CHECK_EQ(beat.LastBeat().Us(), 2500U);
}

KB_TEST("liveness: Watch refuses a zero window and a full monitor") {
  // Static, as a handler's beat is: a monitor keeps the beats' addresses.
  static LivenessBeat a{};
  static LivenessBeat b{};
  static LivenessBeat c{};
  LivenessMonitor<2U> monitor{};
  KB_CHECK_EQ(monitor.Watch(&a, Milliseconds{0U}).Error(), Status::InvalidArgument);
  KB_CHECK_EQ(monitor.Watch(nullptr, Milliseconds{10U}).Error(), Status::InvalidArgument);
  const kb::Result<uint32_t> first = monitor.Watch(&a, Milliseconds{10U});
  const kb::Result<uint32_t> second = monitor.Watch(&b, Milliseconds{10U});
  KB_REQUIRE(first.IsOk() && second.IsOk());
  KB_CHECK_EQ(first.Value(), 0U);
  KB_CHECK_EQ(second.Value(), 1U);
  KB_CHECK_EQ(monitor.Watch(&c, Milliseconds{10U}).Error(), Status::Full);
}

KB_TEST("liveness: a watch is stale after its window without progress; a beat resets it") {
  static LivenessBeat a{};
  static LivenessBeat b{};
  LivenessMonitor<4U> monitor{}; // two free slots stay unused
  KB_REQUIRE(monitor.Watch(&a, Milliseconds{10U}).IsOk());
  KB_REQUIRE(monitor.Watch(&b, Milliseconds{20U}).IsOk());
  const uint64_t t0 = 1000000U;
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0}), 0U);          // first check: the watches start
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 10000U}), 0U); // exactly the window: alive
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 10001U}), 0x1U);
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 20001U}), 0x3U);
  a.Beat(kb::Instant{t0 + 20001U});
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 20002U}), 0x2U); // a made progress
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 30002U}),
              0x2U); // a: exactly its window since the progress
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 30003U}), 0x3U);
  b.Beat(kb::Instant{t0 + 30003U});
  a.Beat(kb::Instant{t0 + 30003U});
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 30004U}), 0U);
}

KB_TEST("liveness: a beat before the first check counts as progress at that check") {
  static LivenessBeat a{};
  a.Beat(kb::Instant{5U});
  LivenessMonitor<1U> monitor{};
  KB_REQUIRE(monitor.Watch(&a, Milliseconds{1U}).IsOk());
  KB_CHECK_EQ(monitor.Check(kb::Instant{5000000U}), 0U);
  KB_CHECK_EQ(monitor.Check(kb::Instant{5002000U}), 0x1U);
}

KB_TEST("liveness: a parked beat is never stale; its window starts again when it resumes") {
  static LivenessBeat a{};
  static LivenessBeat b{};
  LivenessMonitor<2U> monitor{};
  KB_REQUIRE(monitor.Watch(&a, Milliseconds{10U}).IsOk());
  KB_REQUIRE(monitor.Watch(&b, Milliseconds{10U}).IsOk());
  const uint64_t t0 = 2000000U;
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0}), 0U);
  a.Park();
  KB_CHECK(a.Parked());
  KB_CHECK(!b.Parked());
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 50000U}), 0x2U); // only b is late
  a.Resume();
  KB_CHECK(!a.Parked());
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 60000U}), 0x2U); // a starts its window again here
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 70000U}), 0x2U); // exactly the window: alive
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 70001U}), 0x3U);
  a.Park();
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 70002U}), 0x2U);
  a.Beat(kb::Instant{t0 + 70003U}); // a beat ends a park too
  KB_CHECK(!a.Parked());
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 70004U}), 0x2U);
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 80004U}), 0x2U);
  KB_CHECK_EQ(monitor.Check(kb::Instant{t0 + 80005U}), 0x3U);
}

KB_TEST("liveness: Resume() counts as progress even without a beat in between") {
  static LivenessBeat a{};
  LivenessMonitor<1U> monitor{};
  KB_REQUIRE(monitor.Watch(&a, Milliseconds{1U}).IsOk());
  KB_CHECK_EQ(monitor.Check(kb::Instant{100U}), 0U);
  const uint32_t before = a.Beats();
  a.Resume();
  KB_CHECK(a.Beats() != before);
  KB_CHECK_EQ(monitor.Check(kb::Instant{1100U}), 0U); // progress seen: alive
  KB_CHECK_EQ(monitor.Check(kb::Instant{2101U}), 0x1U);
}

KB_TEST("liveness: the verdict kicks only when something is watched and nothing is stale") {
  static LivenessBeat a{};
  static LivenessBeat b{};
  LivenessMonitor<3U> monitor{};
  const kb::handlers::LivenessVerdict none = monitor.Verdict(kb::Instant{1U});
  KB_CHECK(!none.kick);
  KB_CHECK_EQ(none.watched, 0U);
  KB_REQUIRE(monitor.Watch(&a, Milliseconds{1U}).IsOk());
  KB_REQUIRE(monitor.Watch(&b, Milliseconds{1U}).IsOk());
  const kb::handlers::LivenessVerdict first = monitor.Verdict(kb::Instant{1000U});
  KB_CHECK(first.kick);
  KB_CHECK_EQ(first.watched, 2U);
  KB_CHECK_EQ(first.stale, 0U);
  KB_CHECK_EQ(first.parked, 0U);
  b.Park();
  const kb::handlers::LivenessVerdict late = monitor.Verdict(kb::Instant{2001U});
  KB_CHECK(!late.kick);
  KB_CHECK_EQ(late.stale, 0x1U);
  KB_CHECK_EQ(late.parked, 0x2U);
  a.Beat(kb::Instant{2001U});
  const kb::handlers::LivenessVerdict alive = monitor.Verdict(kb::Instant{2002U});
  KB_CHECK(alive.kick);
  KB_CHECK_EQ(alive.parked, 0x2U);
}
