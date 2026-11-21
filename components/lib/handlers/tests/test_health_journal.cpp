// kb::handlers::HealthJournal over a kb::utils::FaultJournal in RAM: a handler entering Faulted and
// serving again writes one record each, nothing else does; liveness stalls are written once per
// newly stale watch; a torn health read and a refusing sink change nothing and are retried; the
// records survive a simulated reset.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>

#include "kb/handlers/common/health_journal.hpp"
#include "kb_test.hpp"

using kb::Status;
using kb::handlers::HealthRecord;
using kb::handlers::HealthState;
using kb::utils::FaultRecord;
using kb::utils::RecordKind;
namespace h = kb::handlers;

namespace {

using Store = kb::utils::RamJournalStore<8U * kb::utils::kFaultRecordBytes>;
using Journal = kb::utils::FaultJournal<Store, 8U>;

/// A sink that refuses while `fail` is set, then forwards to a journal.
struct FlakySink {
  Journal& journal;
  bool fail{false};
  [[nodiscard]] kb::Result<uint32_t> Append(const FaultRecord& r) noexcept {
    return fail ? kb::Result<uint32_t>::Failure(Status::Fault, 9U) : journal.Append(r);
  }
};
static_assert(h::FaultSink<Journal> && h::FaultSink<FlakySink>);
static_assert(!h::FaultSink<int>);

kb::Result<HealthRecord> InState(HealthState state, Status last = Status::Ok, uint32_t detail = 0U,
                                 uint32_t consecutive = 0U) {
  HealthRecord r{};
  r.state = state;
  r.last_status = last;
  r.last_detail = detail;
  r.consecutive = consecutive;
  return kb::Result<HealthRecord>::Success(r);
}

constexpr kb::Instant kAt{123456U};

} // namespace

KB_TEST("health journal: entering Faulted and serving again are the only records") {
  Store store{};
  Journal journal{store};
  KB_REQUIRE(journal.Open().IsOk());
  h::HealthJournal<Journal, 2U> hj{journal};
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Ready), kAt).Value(), 0U);
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Degraded, Status::Nack), kAt).Value(), 0U);
  KB_CHECK_EQ(
      hj.Observe(0U, 7U, InState(HealthState::Faulted, Status::BusError, 0x55U, 3U), kAt).Value(),
      1U);
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Faulted), kAt).Value(), 0U); // still
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Recovering), kAt).Value(), 0U);
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Ready), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(0U, 7U, InState(HealthState::Ready), kAt).Value(), 0U);
  KB_CHECK_EQ(journal.Count(), 2U);
  const FaultRecord fault = journal.Read(1U).Value();
  KB_CHECK(fault.kind == RecordKind::Fault);
  KB_CHECK_EQ(fault.source, 7U);
  KB_CHECK_EQ(fault.status, Status::BusError);
  KB_CHECK_EQ(fault.detail, 0x55U);
  KB_CHECK_EQ(fault.value, 3U);
  KB_CHECK_EQ(fault.time_us, kAt.Us());
  const FaultRecord back = journal.Read(0U).Value();
  KB_CHECK(back.kind == RecordKind::Health);
  KB_CHECK_EQ(back.value, static_cast<uint32_t>(HealthState::Ready));
  KB_CHECK_EQ(back.status, Status::Ok);
}

KB_TEST(
    "health journal: a fault that recovers through Recovering, then degrades, is one edge each") {
  Store store{};
  Journal journal{store};
  KB_REQUIRE(journal.Open().IsOk());
  h::HealthJournal<Journal, 1U> hj{journal};
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Faulted), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Degraded), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Unknown), kAt).Value(), 0U);
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Starting), kAt).Value(), 0U);
  KB_CHECK_EQ(journal.Count(), 2U);
}

KB_TEST("health journal: handlers are tracked apart; an index past N is refused") {
  Store store{};
  Journal journal{store};
  KB_REQUIRE(journal.Open().IsOk());
  h::HealthJournal<Journal, 2U> hj{journal};
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Faulted), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(1U, 2U, InState(HealthState::Faulted), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(1U, 2U, InState(HealthState::Ready), kAt).Value(), 1U);
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Faulted), kAt).Value(), 0U);
  KB_CHECK_EQ(hj.Observe(2U, 3U, InState(HealthState::Faulted), kAt).Error(),
              Status::InvalidArgument);
  KB_CHECK_EQ(journal.Count(), 3U);
}

KB_TEST("health journal: a torn read changes nothing; a refused record is retried") {
  Store store{};
  Journal journal{store};
  KB_REQUIRE(journal.Open().IsOk());
  FlakySink sink{.journal = journal};
  h::HealthJournal<FlakySink, 1U> hj{sink};
  const kb::Result<HealthRecord> torn = kb::Result<HealthRecord>::Failure(Status::Busy, 2U);
  const kb::Result<uint32_t> skipped = hj.Observe(0U, 1U, torn, kAt);
  KB_CHECK_EQ(skipped.Error(), Status::Busy);
  KB_CHECK_EQ(skipped.Detail(), 2U);
  sink.fail = true;
  const kb::Result<uint32_t> refused = hj.Observe(0U, 1U, InState(HealthState::Faulted), kAt);
  KB_CHECK_EQ(refused.Error(), Status::Fault);
  KB_CHECK_EQ(refused.Detail(), 9U);
  // cppcheck-suppress redundantAssignment
  sink.fail = false;
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Faulted), kAt).Value(), 1U); // the retry
  // cppcheck-suppress redundantAssignment
  sink.fail = true;
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Ready), kAt).Error(), Status::Fault);
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  sink.fail = false;
  KB_CHECK_EQ(hj.Observe(0U, 1U, InState(HealthState::Ready), kAt).Value(), 1U);
  KB_CHECK_EQ(journal.Count(), 2U);
}

KB_TEST("health journal: a stall is written once per newly stale watch; a refusal is retried") {
  Store store{};
  Journal journal{store};
  KB_REQUIRE(journal.Open().IsOk());
  FlakySink sink{.journal = journal};
  h::HealthJournal<FlakySink, 1U> hj{sink};
  h::LivenessVerdict v{};
  v.watched = 3U;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 0U);
  v.stale = 0x1U;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 1U);
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 0U); // still stale: no new record
  v.stale = 0x5U;
  sink.fail = true;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Error(), Status::Fault);
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  sink.fail = false;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 1U); // bit 2 is still new
  const FaultRecord stall = journal.Read(0U).Value();
  KB_CHECK(stall.kind == RecordKind::Stall);
  KB_CHECK_EQ(stall.value, 0x4U);
  KB_CHECK_EQ(stall.detail, 0x5U);
  KB_CHECK_EQ(stall.status, Status::Timeout);
  KB_CHECK_EQ(stall.time_us, kAt.Us());
  v.stale = 0x0U;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 0U);
  v.stale = 0x1U;
  KB_CHECK_EQ(hj.ObserveLiveness(v, kAt).Value(), 1U); // stale again after recovering
  KB_CHECK_EQ(journal.Count(), 3U);
}

KB_TEST("health journal: the records survive a reset of the journal object") {
  Store store{};
  {
    Journal journal{store};
    KB_REQUIRE(journal.Open().IsOk());
    h::HealthJournal<Journal, 1U> hj{journal};
    KB_REQUIRE(
        hj.Observe(0U, 4U, InState(HealthState::Faulted, Status::Timeout, 8U, 2U), kAt).IsOk());
  }
  Journal after{store};
  KB_REQUIRE(after.Open().IsOk());
  const kb::Result<FaultRecord> fault = after.FindNewest(RecordKind::Fault);
  KB_REQUIRE(fault.IsOk());
  KB_CHECK_EQ(fault.Value().source, 4U);
  KB_CHECK_EQ(fault.Value().detail, 8U);
}
