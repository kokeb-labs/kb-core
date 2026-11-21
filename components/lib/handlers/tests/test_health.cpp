// kb::handlers::HealthTracker: every transition of its table, every counter and saturation, the
// policy floor.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <limits>

#include "kb/handlers/common/health.hpp"
#include "kb_test.hpp"

using kb::Status;
using kb::handlers::HealthRecord;
using kb::handlers::HealthState;
using kb::handlers::HealthTracker;
namespace h = kb::handlers;

namespace {

HealthRecord Read(const HealthTracker& tracker) noexcept {
  const kb::Result<HealthRecord> r = tracker.Read();
  return r.IsOk() ? r.Value() : HealthRecord{};
}

// A tracker brought to Ready by one completed start at `at_us`.
void StartReady(HealthTracker& tracker, uint64_t at_us) noexcept {
  tracker.BeginStart(false);
  tracker.StartDone(kb::Instant{at_us}, false);
}

} // namespace

KB_TEST("health: SaturatingIncrement adds one and stops at the maximum") {
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  uint32_t v = 5U;
  KB_CHECK_EQ(h::SaturatingIncrement(v), 6U);
  v = kMax - 1U;
  KB_CHECK_EQ(h::SaturatingIncrement(v), kMax);
  v = kMax;
  KB_CHECK_EQ(h::SaturatingIncrement(v), kMax);
  static_assert(h::SaturatingIncrement(kMax) == kMax);
}

KB_TEST("health: a new tracker is Unknown with zero counters, and Read equals Current") {
  const HealthTracker tracker{};
  const HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Unknown);
  KB_CHECK_EQ(r.ok_calls + r.failed_calls + r.refused_calls + r.consecutive, 0U);
  KB_CHECK_EQ(r.starts + r.start_failures + r.recoveries + r.recovery_failures, 0U);
  KB_CHECK_EQ(r.last_status, Status::Ok);
  KB_CHECK(!r.has_good && !r.has_error);
  KB_CHECK_EQ(tracker.Current().state, HealthState::Unknown);
}

KB_TEST("health: BeginStart enters Starting, or Recovering while a recovery runs") {
  HealthTracker tracker{};
  tracker.BeginStart(false);
  KB_CHECK_EQ(Read(tracker).state, HealthState::Starting);
  tracker.BeginStart(true);
  KB_CHECK_EQ(Read(tracker).state, HealthState::Recovering);
}

KB_TEST("health: StartDone is Ready and counts the start (and the recovery)") {
  HealthTracker tracker{};
  tracker.BeginStart(false);
  tracker.StartDone(kb::Instant{1000U}, false);
  HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Ready);
  KB_CHECK_EQ(r.starts, 1U);
  KB_CHECK_EQ(r.recoveries, 0U);
  KB_CHECK_EQ(r.ok_calls, 1U);
  KB_CHECK_EQ(r.last_good_us, 1000U);
  KB_CHECK(r.has_good);
  tracker.BeginStart(true);
  tracker.StartDone(kb::Instant{2000U}, true);
  r = Read(tracker);
  KB_CHECK_EQ(r.starts, 2U);
  KB_CHECK_EQ(r.recoveries, 1U);
  KB_CHECK_EQ(r.last_good_us, 2000U);
}

KB_TEST("health: StartFailed is Faulted and counts the failure (and the failed recovery)") {
  HealthTracker tracker{};
  tracker.BeginStart(false);
  tracker.StartFailed(Status::NoDevice, 7U, kb::Instant{50U}, false);
  HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Faulted);
  KB_CHECK_EQ(r.start_failures, 1U);
  KB_CHECK_EQ(r.recovery_failures, 0U);
  KB_CHECK_EQ(r.consecutive, 1U);
  KB_CHECK_EQ(r.failed_calls, 1U);
  KB_CHECK_EQ(r.last_status, Status::NoDevice);
  KB_CHECK_EQ(r.last_detail, 7U);
  KB_CHECK_EQ(r.last_error_us, 50U);
  KB_CHECK(r.has_error);
  tracker.BeginStart(true);
  tracker.StartFailed(Status::Timeout, 0U, kb::Instant{60U}, true);
  r = Read(tracker);
  KB_CHECK_EQ(r.start_failures, 2U);
  KB_CHECK_EQ(r.recovery_failures, 1U);
  KB_CHECK_EQ(r.consecutive, 2U);
  // A later good start clears the failures in a row.
  tracker.StartDone(kb::Instant{70U}, false);
  KB_CHECK_EQ(Read(tracker).consecutive, 0U);
}

KB_TEST("health: RecordFailure degrades below the threshold and faults at it; RecordOk is Ready "
        "again") {
  HealthTracker tracker{};
  h::HealthPolicy policy{};
  policy.fault_after = 3U;
  tracker.SetPolicy(policy);
  StartReady(tracker, 10U);
  tracker.RecordFailure(Status::BusError, 1U, false, kb::Instant{20U});
  HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Degraded);
  KB_CHECK_EQ(r.consecutive, 1U);
  KB_CHECK_EQ(r.last_error_us, 20U);
  tracker.RecordFailure(Status::BusError, 2U, false, kb::Instant{30U});
  KB_CHECK_EQ(Read(tracker).state, HealthState::Degraded);
  tracker.RecordOk(kb::Instant{40U});
  r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Ready);
  KB_CHECK_EQ(r.consecutive, 0U);
  KB_CHECK_EQ(r.ok_calls, 2U);
  KB_CHECK_EQ(r.last_good_us, 40U);
  for (uint32_t i = 0U; i < 3U; ++i) {
    tracker.RecordFailure(Status::Nack, i, false, kb::Instant{50U});
  }
  r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Faulted);
  KB_CHECK_EQ(r.consecutive, 3U);
  KB_CHECK_EQ(r.failed_calls, 5U);
  KB_CHECK_EQ(r.last_status, Status::Nack);
  KB_CHECK_EQ(r.last_detail, 2U);
}

KB_TEST("health: a fatal failure faults at once") {
  HealthTracker tracker{};
  StartReady(tracker, 1U);
  tracker.RecordFailure(Status::NotReady, 9U, true, kb::Instant{2U});
  const HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Faulted);
  KB_CHECK_EQ(r.consecutive, 1U);
}

KB_TEST("health: outside the serving states RecordOk and RecordFailure count but do not move the "
        "state") {
  HealthTracker tracker{};
  tracker.RecordFailure(Status::BusError, 0U, true, kb::Instant{1U});
  HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Unknown);
  KB_CHECK_EQ(r.failed_calls, 1U);
  KB_CHECK_EQ(r.consecutive, 1U);
  tracker.Fault(Status::BusError, 4U, kb::Instant{2U});
  tracker.RecordOk(kb::Instant{3U});
  r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Faulted);
  KB_CHECK_EQ(r.ok_calls, 1U);
  KB_CHECK_EQ(r.consecutive, 0U);
}

KB_TEST("health: RecordRefused records the error but not the state or the failures in a row") {
  HealthTracker tracker{};
  StartReady(tracker, 1U);
  tracker.RecordFailure(Status::BusError, 0U, false, kb::Instant{2U});
  tracker.RecordRefused(Status::InvalidArgument, 11U, kb::Instant{3U});
  const HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Degraded);
  KB_CHECK_EQ(r.consecutive, 1U);
  KB_CHECK_EQ(r.refused_calls, 1U);
  KB_CHECK_EQ(r.failed_calls, 1U);
  KB_CHECK_EQ(r.last_status, Status::InvalidArgument);
  KB_CHECK_EQ(r.last_detail, 11U);
  KB_CHECK_EQ(r.last_error_us, 3U);
  KB_CHECK(r.has_error);
}

KB_TEST("health: Fault marks Faulted from any state and counts a failure") {
  HealthTracker tracker{};
  StartReady(tracker, 1U);
  tracker.Fault(Status::BusError, 8U, kb::Instant{5U});
  const HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Faulted);
  KB_CHECK_EQ(r.failed_calls, 1U);
  KB_CHECK_EQ(r.last_detail, 8U);
  KB_CHECK_EQ(r.last_error_us, 5U);
}

KB_TEST("health: Stopped returns to Unknown, clears the failures in a row, keeps the counters") {
  HealthTracker tracker{};
  StartReady(tracker, 1U);
  tracker.RecordFailure(Status::BusError, 0U, false, kb::Instant{2U});
  tracker.Stopped();
  const HealthRecord r = Read(tracker);
  KB_CHECK_EQ(r.state, HealthState::Unknown);
  KB_CHECK_EQ(r.consecutive, 0U);
  KB_CHECK_EQ(r.starts, 1U);
  KB_CHECK_EQ(r.failed_calls, 1U);
  KB_CHECK(r.has_good && r.has_error);
}

KB_TEST("health: fault_after 0 is taken as 1") {
  HealthTracker tracker{};
  h::HealthPolicy policy{};
  policy.fault_after = 0U;
  tracker.SetPolicy(policy);
  StartReady(tracker, 1U);
  tracker.RecordFailure(Status::BusError, 0U, false, kb::Instant{2U});
  KB_CHECK_EQ(Read(tracker).state, HealthState::Faulted);

  HealthTracker one{};
  policy.fault_after = 2U;
  one.SetPolicy(policy);
  StartReady(one, 1U);
  one.RecordFailure(Status::BusError, 0U, false, kb::Instant{2U});
  KB_CHECK_EQ(Read(one).state, HealthState::Degraded);
}

KB_TEST("health: every publish is visible to Read at once (single writer, no torn copy)") {
  HealthTracker tracker{};
  for (uint32_t i = 0U; i < 100U; ++i) {
    tracker.RecordOk(kb::Instant{static_cast<uint64_t>(i)});
    const kb::Result<HealthRecord> r = tracker.Read();
    KB_REQUIRE(r.IsOk());
    KB_CHECK_EQ(r.Value().ok_calls, i + 1U);
    KB_CHECK_EQ(r.Value().last_good_us, static_cast<uint64_t>(i));
  }
}
