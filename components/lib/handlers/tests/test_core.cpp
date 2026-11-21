// kb::handlers::HandlerCore on the none backend (deterministic fake time): the start-up runner
// stage by stage, the report, retry, deinitialisation, recovery with backoff, the served calls and
// the health they record; and the vocabulary helpers of types.hpp and driver_result.hpp.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
// cppcheck reads the test scripts' fields as unused: the handler under test reads them through a
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "handler_test_support.hpp"
#include "kb/rtos/none/control.hpp"
#include "kb/rtos/time.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
using kbt::Command;
using kbt::Plus;
using kbt::Reading;
using kbt::Script;
using kbt::Toy;
namespace h = kb::handlers;
namespace feat = kbt::feat;

namespace {

constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();

const h::ServiceKey& Key() noexcept {
  static const h::ServiceKey kKey = h::ServiceKeyTestPeer::Make();
  return kKey;
}

template <typename H>
h::HealthRecord HealthOf(const H& handler) noexcept {
  const kb::Result<h::HealthRecord> r = handler.Health();
  return r.IsOk() ? r.Value() : h::HealthRecord{};
}

template <typename H>
h::StartupReport ReportOf(const H& handler) noexcept {
  const kb::Result<h::StartupReport> r = handler.Startup();
  return r.IsOk() ? r.Value() : h::StartupReport{};
}

h::HandlerConfig Config() noexcept {
  return h::HandlerConfig{};
}

void Elapse(uint32_t ms) noexcept {
  kb::rtos::none::AdvanceTicks(ms);
}

} // namespace

// --- types.hpp ---------------------------------------------------------------------------------

KB_TEST("types: every health state has a name; an unknown value is Unknown") {
  // Runtime values: constant arguments would let the compiler fold the calls.
  std::array<uint8_t, 7> raw{0U, 1U, 2U, 3U, 4U, 5U, 77U};
  const std::array<const char*, 7> names{"Unknown", "Starting",   "Ready",  "Degraded",
                                         "Faulted", "Recovering", "Unknown"};
  for (std::size_t i = 0U; i < raw.size(); ++i) {
    KB_CHECK(std::strcmp(h::ToString(static_cast<h::HealthState>(raw[i])), names[i]) == 0);
  }
  static_assert(h::ToString(h::HealthState::Faulted)[0] == 'F');
}

KB_TEST("types: only Ready and Degraded serve") {
  std::array<h::HealthState, 6> states{h::HealthState::Unknown, h::HealthState::Starting,
                                       h::HealthState::Ready,   h::HealthState::Degraded,
                                       h::HealthState::Faulted, h::HealthState::Recovering};
  uint32_t serving = 0U;
  for (const h::HealthState s : states) {
    // cppcheck-suppress useStlAlgorithm
    serving += h::IsServing(s) ? 1U : 0U;
  }
  KB_CHECK_EQ(serving, 2U);
  KB_CHECK(h::IsServing(states[2]) && h::IsServing(states[3]));
  KB_CHECK(!h::IsServing(states[0]) && !h::IsServing(states[4]) && !h::IsServing(states[5]));
}

KB_TEST("types: lowest and highest kind bits") {
  std::array<uint16_t, 5> kinds{0U, h::stage::kPower,
                                static_cast<uint16_t>(h::stage::kBoot | h::stage::kLink),
                                h::stage::kAll, static_cast<uint16_t>(0x0100U)};
  KB_CHECK_EQ(h::LowestKind(kinds[0]), 0U);
  KB_CHECK_EQ(h::HighestKind(kinds[0]), 0U);
  KB_CHECK_EQ(h::LowestKind(kinds[1]), h::stage::kPower);
  KB_CHECK_EQ(h::HighestKind(kinds[1]), h::stage::kPower);
  KB_CHECK_EQ(h::LowestKind(kinds[2]), h::stage::kBoot);
  KB_CHECK_EQ(h::HighestKind(kinds[2]), h::stage::kLink);
  KB_CHECK_EQ(h::LowestKind(kinds[3]), h::stage::kPower);
  KB_CHECK_EQ(h::HighestKind(kinds[3]), h::stage::kVerify);
  KB_CHECK_EQ(h::LowestKind(kinds[4]), 0U); // above the known kinds
  KB_CHECK_EQ(h::HighestKind(kinds[4]), 0U);
}

KB_TEST("types: IsValidPlan accepts ordered plans and refuses bad ones") {
  using h::Stage;
  using h::StageMode;
  std::array<Stage, 2> good{};
  good[0] = Stage{.kinds = h::stage::kPower, .mode = StageMode::Sync, .optional = true};
  good[1] = Stage{.kinds = static_cast<uint16_t>(h::stage::kIdentify | h::stage::kConfigure),
                  .mode = StageMode::Polled,
                  .optional = false};
  KB_CHECK(h::IsValidPlan(good));

  std::array<Stage, 2> reversed = good;
  reversed[0] = good[1];
  reversed[1] = good[0];
  KB_CHECK(!h::IsValidPlan(reversed));

  std::array<Stage, 2> repeated = good;
  repeated[1] = Stage{.kinds = h::stage::kPower, .mode = StageMode::Sync, .optional = false};
  KB_CHECK(!h::IsValidPlan(repeated)); // a kind twice

  std::array<Stage, 2> overlapping = good;
  overlapping[0] = Stage{.kinds = static_cast<uint16_t>(h::stage::kPower | h::stage::kConfigure),
                         .mode = StageMode::Sync,
                         .optional = false};
  KB_CHECK(!h::IsValidPlan(overlapping)); // second stage starts below the first one's highest kind

  std::array<Stage, 1> empty_kinds{};
  KB_CHECK(!h::IsValidPlan(empty_kinds)); // kinds 0

  std::array<Stage, 2> unknown_first = good;
  unknown_first[0] =
      Stage{.kinds = static_cast<uint16_t>(0x0200U), .mode = StageMode::Sync, .optional = false};
  KB_CHECK(
      !h::IsValidPlan(unknown_first)); // an unknown bit; the next stage is checked against no kind

  std::array<Stage, 0> none{};
  KB_CHECK(!h::IsValidPlan(none)); // no stage

  std::array<Stage, 9> too_many{};
  for (std::size_t i = 0U; i < too_many.size(); ++i) {
    too_many[i] = Stage{
        .kinds = static_cast<uint16_t>(1U << (i % 8U)), .mode = StageMode::Sync, .optional = false};
  }
  KB_CHECK(!h::IsValidPlan(too_many)); // more than kMaxStages

  std::array<Stage, 8> all_kinds{};
  for (std::size_t i = 0U; i < all_kinds.size(); ++i) {
    all_kinds[i] =
        Stage{.kinds = static_cast<uint16_t>(1U << i), .mode = StageMode::Sync, .optional = false};
  }
  KB_CHECK(h::IsValidPlan(all_kinds));
  static_assert(h::IsValidPlan(kbt::PlanThree::kPlan));
}

// --- driver_result.hpp -------------------------------------------------------------------------

namespace {

enum class DrvError : uint8_t { Ok = 0, Bus = 3, Timeout = 9, Reset = 200 };

struct DrvStatus {
  DrvError error{DrvError::Ok};
  [[nodiscard]] bool ok() const noexcept {
    return error == DrvError::Ok;
  }
};

struct DrvValue {
  uint16_t value{0U};
  DrvError error{DrvError::Ok};
  [[nodiscard]] bool ok() const noexcept {
    return error == DrvError::Ok;
  }
};

[[nodiscard]] Status MapDrv(DrvError e) noexcept {
  Status s = Status::Fault;
  switch (e) {
  case DrvError::Ok:
    s = Status::Ok;
    break;
  case DrvError::Bus:
    s = Status::BusError;
    break;
  case DrvError::Timeout:
    s = Status::Timeout;
    break;
  case DrvError::Reset:
    s = Status::NotReady;
    break;
  }
  return s;
}

static_assert(h::DriverError<DrvError>);
static_assert(h::DriverStatusResult<DrvStatus>);
static_assert(h::DriverValueResult<DrvValue>);
static_assert(!h::DriverValueResult<DrvStatus>);

} // namespace

KB_TEST("driver_result: Lift, LiftStatus, Forward and DetailOf") {
  DrvValue good{};
  good.value = 0x1234U;
  const kb::Result<uint16_t> lifted = h::Lift(good, &MapDrv);
  KB_REQUIRE(lifted.IsOk());
  KB_CHECK_EQ(lifted.Value(), 0x1234U);

  DrvValue bad{};
  bad.error = DrvError::Reset;
  const kb::Result<uint16_t> failed = h::Lift(bad, &MapDrv);
  KB_CHECK_EQ(failed.Error(), Status::NotReady);
  KB_CHECK_EQ(failed.Detail(), 200U);

  DrvStatus ok_status{};
  KB_CHECK(h::LiftStatus(ok_status, &MapDrv).IsOk());
  DrvStatus bus{};
  bus.error = DrvError::Bus;
  const kb::Result<void> bus_failed = h::LiftStatus(bus, &MapDrv);
  KB_CHECK_EQ(bus_failed.Error(), Status::BusError);
  KB_CHECK_EQ(bus_failed.Detail(), 3U);

  const kb::Result<Reading> forwarded = h::Forward<Reading>(failed);
  KB_CHECK_EQ(forwarded.Error(), Status::NotReady);
  KB_CHECK_EQ(forwarded.Detail(), 200U);
  const kb::Result<void> forwarded_void = h::Forward<void>(bus_failed);
  KB_CHECK_EQ(forwarded_void.Error(), Status::BusError);
  KB_CHECK_EQ(forwarded_void.Detail(), 3U);

  DrvError timeout = DrvError::Timeout;
  KB_CHECK_EQ(h::DetailOf(timeout), 9U);
}

// --- start-up runner ---------------------------------------------------------------------------

KB_TEST("core: before a start every served call is NotReady and touches no driver") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  KB_CHECK_EQ(t.Execute(Key(), Command{.op = 1U, .arg = 2U}, now).Error(), Status::NotReady);
  KB_CHECK_EQ(t.Sample(Key(), now).Error(), Status::NotReady);
  KB_CHECK_EQ(t.Latest().Error(), Status::NotReady);
  KB_CHECK_EQ(s.runs, 0U);
  KB_CHECK_EQ(s.measures, 0U);
  KB_CHECK_EQ(s.emplaced, 0U);
  KB_CHECK(t.Driver() == nullptr);
  KB_CHECK(Toy<kbt::PlanSync>::Measures());
  const h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Unknown);
  KB_CHECK_EQ(health.ok_calls, 0U);
  KB_CHECK_EQ(health.failed_calls, 0U);
  KB_CHECK(!health.has_good && !health.has_error);
  const kb::Result<h::StartupReport> report = t.Startup();
  KB_REQUIRE(report.IsOk());
  KB_CHECK_EQ(report.Value().runs, 0U);
  KB_CHECK_EQ(report.Value().stages, 0U);
  KB_CHECK(!report.Value().complete);
  KB_CHECK_EQ(report.Value().status, Status::NotReady); // the default report, not zero words
}

KB_TEST("core: a one-stage sync plan starts in one step, reports, and is idempotent once Ready") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  const kb::Result<h::StartProgress> step = t.StartStep(Key(), now);
  KB_REQUIRE(step.IsOk());
  KB_CHECK_EQ(step.Value(), h::StartProgress::Done);
  KB_CHECK_EQ(s.emplaced, 1U);
  KB_CHECK_EQ(s.stages[0].calls, 1U);
  KB_CHECK_EQ(s.stages[0].last.index, 0U);
  KB_CHECK_EQ(s.stages[0].last.calls, 0U);
  KB_CHECK(s.stages[0].last.now == now);
  KB_CHECK(s.stages[0].last.started == now);
  KB_CHECK(t.Driver() != nullptr);

  const h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.runs, 1U);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.stage, 0U);
  KB_CHECK_EQ(report.kinds, static_cast<uint16_t>(h::stage::kIdentify | h::stage::kConfigure));
  KB_CHECK_EQ(report.status, Status::Ok);
  KB_CHECK_EQ(report.detail, 0U);
  KB_CHECK_EQ(report.stage_us[0], 0U);
  KB_CHECK(report.complete);

  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  KB_CHECK_EQ(health.starts, 1U);
  KB_CHECK_EQ(health.ok_calls, 1U);
  KB_CHECK(health.has_good);
  KB_CHECK_EQ(health.last_good_us, now.Us());

  // Started: StartStep and EnsureInitialized are Done/Ok at once, with no stage call and no new
  // run.
  const kb::Result<h::StartProgress> again = t.StartStep(Key(), Plus(now, 5U));
  KB_REQUIRE(again.IsOk());
  KB_CHECK_EQ(again.Value(), h::StartProgress::Done);
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.stages[0].calls, 1U);
  KB_CHECK_EQ(s.emplaced, 1U);
  KB_CHECK_EQ(ReportOf(t).runs, 1U);
  health = HealthOf(t);
  KB_CHECK_EQ(health.starts, 1U);
  KB_CHECK(t.Settings().poll == Milliseconds{1U});
}

KB_TEST("core: a polled stage is stepped until done; per-stage time and StageCall") {
  Script s{};
  s.stages[1].pending = 2U;
  Toy<kbt::PlanThree> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  kb::Result<h::StartProgress> r = t.StartStep(Key(), t0); // stage 0 done, stage 1 next
  KB_REQUIRE(r.IsOk());
  KB_CHECK_EQ(r.Value(), h::StartProgress::Pending);
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Starting);
  r = t.StartStep(Key(), Plus(t0, 1000U)); // stage 1, call 0
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Pending));
  r = t.StartStep(Key(), Plus(t0, 2000U)); // stage 1, call 1
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Pending));
  r = t.StartStep(Key(), Plus(t0, 3000U)); // stage 1 done
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Pending));
  KB_CHECK_EQ(s.stages[1].calls, 3U);
  KB_CHECK_EQ(s.stages[1].last.index, 1U);
  KB_CHECK_EQ(s.stages[1].last.calls, 2U);
  KB_CHECK(s.stages[1].last.started == t0);
  KB_CHECK(s.stages[1].last.now == Plus(t0, 3000U));
  KB_CHECK_EQ(s.stages[2].calls, 0U);
  r = t.StartStep(Key(), Plus(t0, 4000U)); // stage 2 done
  KB_REQUIRE(r.IsOk());
  KB_CHECK_EQ(r.Value(), h::StartProgress::Done);
  const h::StartupReport report = ReportOf(t);
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stage, 2U);
  KB_CHECK_EQ(report.kinds, h::stage::kConfigure);
  KB_CHECK_EQ(report.stages, 3U);
  KB_CHECK_EQ(report.stage_us[0], 0U);
  KB_CHECK_EQ(report.stage_us[1], 3000U);
  KB_CHECK_EQ(report.stage_us[2], 1000U);
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Ready);
}

KB_TEST("core: Pending from a sync stage is a Fault; the driver is aborted and destroyed") {
  Script s{};
  s.stages[0].pending = 1U;
  s.stages[0].detail = 99U;
  Toy<kbt::PlanSync> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), now);
  KB_CHECK_EQ(r.Error(), Status::Fault);
  KB_CHECK_EQ(s.aborts, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK(t.Driver() == nullptr);
  const h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.status, Status::Fault);
  KB_CHECK_EQ(report.stage, 0U);
  KB_CHECK_EQ(report.detail, 0U);
  KB_CHECK(!report.complete);
  const h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.start_failures, 1U);
  KB_CHECK_EQ(health.last_status, Status::Fault);
}

KB_TEST("core: a polled stage past its deadline is a Timeout") {
  Script s{};
  s.stages[0].pending = kbt::kAlways;
  s.stages[0].deadline_ms = 5U;
  Toy<kbt::PlanPolled> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(t.StartStep(Key(), t0).IsOk());
  KB_CHECK(t.StartStep(Key(), Plus(t0, 4999U)).IsOk());
  const kb::Result<h::StartProgress> late = t.StartStep(Key(), Plus(t0, 5000U));
  KB_CHECK_EQ(late.Error(), Status::Timeout);
  KB_CHECK_EQ(s.stages[0].calls, 3U);
  const h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.status, Status::Timeout);
  KB_CHECK_EQ(report.stage, 0U);
  KB_CHECK_EQ(report.kinds, h::stage::kBoot);
  KB_CHECK_EQ(s.aborts, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
}

KB_TEST("core: a failing stage names stage, kinds, status and detail; the next start begins at "
        "stage 0") {
  Script s{};
  s.stages[2].fail = Status::NoDevice;
  s.stages[2].fail_times = 1U;
  s.stages[2].detail = 0x42U;
  Toy<kbt::PlanThree> t{s, Config()};
  const kb::Result<void> first = t.EnsureInitialized(Key());
  KB_CHECK_EQ(first.Error(), Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), 0x42U);
  h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.stage, 2U);
  KB_CHECK_EQ(report.kinds, h::stage::kConfigure);
  KB_CHECK_EQ(report.status, Status::NoDevice);
  KB_CHECK_EQ(report.detail, 0x42U);
  KB_CHECK_EQ(report.runs, 1U);
  KB_CHECK(!report.complete);
  KB_CHECK_EQ(s.aborts, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.last_status, Status::NoDevice);
  KB_CHECK_EQ(health.last_detail, 0x42U);
  KB_CHECK_EQ(health.start_failures, 1U);
  KB_CHECK_EQ(health.consecutive, 1U);
  KB_CHECK(health.has_error);

  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.stages[0].calls, 2U); // retried from the first stage
  KB_CHECK_EQ(s.stages[1].calls, 2U);
  KB_CHECK_EQ(s.stages[2].calls, 2U);
  KB_CHECK_EQ(s.emplaced, 2U);
  report = ReportOf(t);
  KB_CHECK_EQ(report.runs, 2U);
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.status, Status::Ok);
  KB_CHECK_EQ(report.detail, 0U);
  health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  KB_CHECK_EQ(health.starts, 1U);
  KB_CHECK_EQ(health.consecutive, 0U);
}

KB_TEST("core: optional stages are skipped when not enabled, also at the end of the plan") {
  Script s{};
  s.stages[1].enabled = false;
  s.stages[2].enabled = false;
  Toy<kbt::PlanTrailing> t{s, Config()};
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), kb::rtos::Now());
  KB_REQUIRE(r.IsOk());
  KB_CHECK_EQ(r.Value(), h::StartProgress::Done);
  KB_CHECK_EQ(s.stages[0].calls, 1U);
  KB_CHECK_EQ(s.stages[1].calls, 0U);
  KB_CHECK_EQ(s.stages[2].calls, 0U);
  const h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.stage, 0U);
  KB_CHECK_EQ(report.kinds, h::stage::kIdentify);
  KB_CHECK(report.complete);
}

KB_TEST(
    "core: skipped stages before and between required ones; a required stage is never skipped") {
  Script s{};
  s.stages[0].enabled = false; // optional: skipped before the first step
  s.stages[1].enabled = false; // required: runs anyway
  s.stages[2].enabled = false; // optional: skipped after stage 1
  Toy<kbt::PlanMiddle> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  kb::Result<h::StartProgress> r = t.StartStep(Key(), t0);
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Pending));
  KB_CHECK_EQ(s.stages[0].calls, 0U);
  KB_CHECK_EQ(s.stages[1].calls, 1U);
  KB_CHECK_EQ(s.stages[2].calls, 0U);
  r = t.StartStep(Key(), Plus(t0, 700U));
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Done));
  KB_CHECK_EQ(s.stages[3].calls, 1U);
  KB_CHECK(s.stages[3].last.started == t0);
  KB_CHECK_EQ(ReportOf(t).stage_us[3], 700U);
}

KB_TEST("core: without StageEnabled every optional stage runs") {
  Script s{};
  s.stages[1].enabled = false; // not consulted
  s.stages[2].enabled = false;
  Toy<kbt::PlanTrailing, feat::kAll & ~feat::kStageEnabled> t{s, Config()};
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.stages[1].calls, 1U);
  KB_CHECK_EQ(s.stages[2].calls, 1U);
}

KB_TEST("core: a plan of skipped stages and no driver is Ready but serves nothing") {
  Script s{};
  s.emplace = false;
  s.stages[0].enabled = false;
  s.stages[1].enabled = false;
  Toy<kbt::PlanOptionalOnly> t{s, Config()};
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), kb::rtos::Now());
  KB_REQUIRE(r.IsOk());
  KB_CHECK_EQ(r.Value(), h::StartProgress::Done);
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Ready);
  KB_CHECK(t.Driver() == nullptr);
  KB_CHECK_EQ(t.Execute(Key(), Command{}, kb::rtos::Now()).Error(), Status::NotReady);
  KB_CHECK_EQ(t.Sample(Key(), kb::rtos::Now()).Error(), Status::NotReady);
  KB_CHECK_EQ(s.runs, 0U);
  KB_CHECK_EQ(s.measures, 0U);
  KB_CHECK_EQ(ReportOf(t).stage, 0U);
}

KB_TEST("core: a binding that builds no driver fails the start with Fault, with no abort") {
  Script s{};
  s.emplace = false;
  Toy<kbt::PlanSync> t{s, Config()};
  KB_CHECK_EQ(t.StartStep(Key(), kb::rtos::Now()).Error(), Status::Fault);
  KB_CHECK_EQ(s.aborts, 0U);
  KB_CHECK_EQ(s.stages[0].calls, 0U);
  KB_CHECK_EQ(ReportOf(t).status, Status::Fault);
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Faulted);
  // Faulted by the start (no driver): the next step starts a new run.
  KB_CHECK_EQ(t.StartStep(Key(), kb::rtos::Now()).Error(), Status::Fault);
  KB_CHECK_EQ(ReportOf(t).runs, 2U);
  KB_CHECK_EQ(s.emplace_calls, 2U);
}

KB_TEST("core: a failed start destroys the driver also without AbortStartup") {
  Script s{};
  s.stages[0].fail = Status::BusError;
  s.stages[0].fail_times = 1U;
  Toy<kbt::PlanSync, feat::kAll & ~feat::kAbort> t{s, Config()};
  KB_CHECK_EQ(t.StartStep(Key(), kb::rtos::Now()).Error(), Status::BusError);
  KB_CHECK_EQ(s.aborts, 0U);
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK(t.Driver() == nullptr);
}

KB_TEST("core: EnsureInitialized waits the poll interval between steps (0 is taken as 1 ms)") {
  Script s{};
  s.stages[0].pending = 3U;
  h::HandlerConfig config = Config();
  config.poll = Milliseconds{0U};
  Toy<kbt::PlanPolled> t{s, config};
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.stages[0].calls, 4U);
  // Three sleeps of at least 1 ms: two ticks each on the 1 kHz fake clock.
  KB_CHECK_EQ(kb::utils::ElapsedUs(kb::rtos::Now(), t0), 6000U);

  Script s3{};
  s3.stages[0].pending = 2U;
  config.poll = Milliseconds{3U};
  Toy<kbt::PlanPolled> t3{s3, config};
  const kb::Instant t1 = kb::rtos::Now();
  KB_CHECK(t3.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(kb::utils::ElapsedUs(kb::rtos::Now(), t1), 8000U);
}

KB_TEST("core: EnsureInitialized gives Timeout when the plan's budget is spent") {
  Script s{};
  s.elapse = &Elapse;
  s.stages[0].deadline_ms = 1U;
  s.stages[0].elapse_ms = 50U; // a slow sync call
  s.stages[1].deadline_ms = 10U;
  s.stages[1].pending = kbt::kAlways;
  Toy<kbt::PlanSyncPolled> t{s, Config()}; // budget: 1 + 1 + 10 + 1 ms
  const kb::Result<void> r = t.EnsureInitialized(Key());
  KB_CHECK_EQ(r.Error(), Status::Timeout);
  KB_CHECK_EQ(s.stages[1].calls, 0U);
  const h::StartupReport report = ReportOf(t);
  KB_CHECK_EQ(report.status, Status::Timeout);
  KB_CHECK_EQ(report.stage, 1U);
  KB_CHECK_EQ(report.kinds, h::stage::kConfigure);
  KB_CHECK_EQ(s.aborts, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
  const h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.last_status, Status::Timeout);
}

KB_TEST("core: the start budget saturates for very long deadlines") {
  Script s{};
  s.stages[0].deadline_ms = kU32Max;
  s.stages[1].deadline_ms = kU32Max;
  Toy<kbt::PlanTwoSync> t{s, Config()};
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.stages[1].calls, 1U);
}

KB_TEST("core: a stage time beyond 32 bits of microseconds saturates in the report") {
  Script s{};
  s.stages[0].pending = 1U;
  s.stages[0].deadline_ms = kU32Max;
  Toy<kbt::PlanPolled> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(t.StartStep(Key(), t0).IsOk());
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), Plus(t0, 5000000000ULL));
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Done));
  KB_CHECK_EQ(ReportOf(t).stage_us[0], kU32Max);
}

KB_TEST("core: StartStep on a handler faulted while running is NotReady; Recover restarts it") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), now).IsOk());
  t.Fault(Key(), Status::BusError, 3U, now);
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.last_status, Status::BusError);
  KB_CHECK_EQ(health.last_detail, 3U);
  KB_CHECK_EQ(health.failed_calls, 1U);
  KB_CHECK_EQ(t.StartStep(Key(), now).Error(), Status::NotReady);
  KB_CHECK_EQ(t.EnsureInitialized(Key()).Error(), Status::NotReady);
  KB_CHECK_EQ(t.Execute(Key(), Command{}, now).Error(), Status::NotReady);
  KB_CHECK_EQ(s.stages[0].calls, 1U);
  KB_CHECK_EQ(s.runs, 0U);

  KB_CHECK(t.Recover(Key(), now).IsOk());
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Recovering);
  KB_CHECK_EQ(s.recovers, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK(t.Driver() == nullptr);
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), now);
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Done));
  health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  KB_CHECK_EQ(health.recoveries, 1U);
  KB_CHECK_EQ(health.starts, 2U);
}

KB_TEST("core: Fault() before any start leaves no driver, so StartStep starts") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  t.Fault(Key(), Status::BusError, 0U, kb::rtos::Now());
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Faulted);
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), kb::rtos::Now());
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Done));
}

KB_TEST("core: Deinitialize runs StopDriver, destroys the driver and keeps the counters") {
  Script s{};
  s.stop_status = Status::BusError;
  Toy<kbt::PlanSync> t{s, Config()};
  KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
  const kb::Result<void> stopped = t.Deinitialize(Key());
  KB_CHECK_EQ(stopped.Error(), Status::BusError);
  KB_CHECK_EQ(s.stops, 1U);
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK(t.Driver() == nullptr);
  const h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Unknown);
  KB_CHECK_EQ(health.starts, 1U);
  KB_CHECK_EQ(health.ok_calls, 1U);
  KB_CHECK_EQ(t.Execute(Key(), Command{}, kb::rtos::Now()).Error(), Status::NotReady);
  // No driver: nothing to stop.
  KB_CHECK(t.Deinitialize(Key()).IsOk());
  KB_CHECK_EQ(s.stops, 1U);
  // A new start works.
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(s.emplaced, 2U);
}

KB_TEST("core: Deinitialize without StopDriver, and in the middle of a run") {
  Script s{};
  s.stages[0].pending = 5U;
  Toy<kbt::PlanPolled, feat::kAll & ~(feat::kStop | feat::kRecover)> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(t.StartStep(Key(), t0).IsOk());
  KB_CHECK(t.Deinitialize(Key()).IsOk());
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Unknown);
  // The interrupted run does not continue: the next step begins a new run at the first call.
  KB_CHECK(t.StartStep(Key(), t0).IsOk());
  KB_CHECK_EQ(ReportOf(t).runs, 2U);
  KB_CHECK_EQ(s.stages[0].last.calls, 0U);
}

KB_TEST("core: Recover is NotReady before a start and after Deinitialize") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  KB_CHECK_EQ(t.Recover(Key(), kb::rtos::Now()).Error(), Status::NotReady);
  KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
  KB_REQUIRE(t.Deinitialize(Key()).IsOk());
  KB_CHECK_EQ(t.Recover(Key(), kb::rtos::Now()).Error(), Status::NotReady);
  KB_CHECK_EQ(s.recovers, 0U);
}

KB_TEST("core: Recover uses RecoverDriver when declared, else StopDriver, else nothing") {
  {
    Script s{};
    s.recover_status = Status::BusError; // ignored: the plan runs again anyway
    Toy<kbt::PlanSync> t{s, Config()};
    KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
    KB_CHECK(t.Recover(Key(), kb::rtos::Now()).IsOk());
    KB_CHECK_EQ(s.recovers, 1U);
    KB_CHECK_EQ(s.stops, 0U);
    KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  }
  {
    Script s{};
    Toy<kbt::PlanSync, feat::kAll & ~feat::kRecover> t{s, Config()};
    KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
    KB_CHECK(t.Recover(Key(), kb::rtos::Now()).IsOk());
    KB_CHECK_EQ(s.recovers, 0U);
    KB_CHECK_EQ(s.stops, 1U);
  }
  {
    Script s{};
    Toy<kbt::PlanSync, feat::kNone> t{s, Config()};
    KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
    KB_CHECK(t.Recover(Key(), kb::rtos::Now()).IsOk());
    KB_CHECK_EQ(s.destroyed, 1U);
    KB_CHECK(t.EnsureInitialized(Key()).IsOk());
    KB_CHECK_EQ(HealthOf(t).recoveries, 1U);
  }
}

KB_TEST("core: failed recoveries back off exponentially up to max_backoff; success resets the "
        "attempts") {
  Script s{};
  h::HandlerConfig config = Config();
  config.recovery.backoff = Milliseconds{10U};
  config.recovery.max_backoff = Milliseconds{35U};
  config.recovery.max_attempts = 0U;
  Toy<kbt::PlanSync> t{s, config};
  const kb::Instant t0 = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), t0).IsOk());
  s.stages[0].fail = Status::NoDevice;
  s.stages[0].fail_times = kbt::kAlways;

  // Attempt 1 at t0, then waits of 10, 20, 35 (40 capped), 35 ms.
  const std::array<uint64_t, 4> waits_us{10000U, 20000U, 35000U, 35000U};
  uint64_t at = t0.Us();
  KB_CHECK(t.Recover(Key(), kb::Instant{at}).IsOk()); // the first recovery has no backoff
  KB_CHECK_EQ(t.StartStep(Key(), kb::Instant{at}).Error(), Status::NoDevice);
  for (const uint64_t wait : waits_us) {
    KB_CHECK_EQ(t.Recover(Key(), kb::Instant{at + wait - 1U}).Error(), Status::Busy);
    at += wait;
    KB_CHECK(t.Recover(Key(), kb::Instant{at}).IsOk());
    KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Recovering);
    KB_CHECK_EQ(t.StartStep(Key(), kb::Instant{at}).Error(), Status::NoDevice);
  }
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.recovery_failures, 5U);
  KB_CHECK_EQ(health.start_failures, 5U);
  KB_CHECK_EQ(health.recoveries, 0U);

  // A successful recovery counts and resets the backoff to its first step.
  // cppcheck-suppress redundantAssignment
  s.stages[0].fail_times = 0U;
  at += 35000U;
  KB_CHECK(t.Recover(Key(), kb::Instant{at}).IsOk());
  KB_CHECK(t.StartStep(Key(), kb::Instant{at}).IsOk());
  health = HealthOf(t);
  KB_CHECK_EQ(health.recoveries, 1U);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  // cppcheck-suppress unreadVariable
  s.stages[0].fail_times = kbt::kAlways;
  KB_CHECK(t.Recover(Key(), kb::Instant{at}).IsOk()); // attempts were reset: no backoff
  KB_CHECK_EQ(t.StartStep(Key(), kb::Instant{at}).Error(), Status::NoDevice);
  KB_CHECK_EQ(t.Recover(Key(), kb::Instant{at + 9999U}).Error(), Status::Busy);
  KB_CHECK(t.Recover(Key(), kb::Instant{at + 10000U}).IsOk());
}

KB_TEST("core: a first backoff above max_backoff is capped") {
  Script s{};
  h::HandlerConfig config = Config();
  config.recovery.backoff = Milliseconds{50U};
  config.recovery.max_backoff = Milliseconds{20U};
  Toy<kbt::PlanSync> t{s, config};
  const kb::Instant t0 = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), t0).IsOk());
  s.stages[0].fail = Status::NoDevice;
  // cppcheck-suppress unreadVariable
  s.stages[0].fail_times = kbt::kAlways;
  KB_CHECK(t.Recover(Key(), t0).IsOk());
  KB_CHECK(!t.StartStep(Key(), t0).IsOk());
  KB_CHECK_EQ(t.Recover(Key(), Plus(t0, 19999U)).Error(), Status::Busy);
  KB_CHECK(t.Recover(Key(), Plus(t0, 20000U)).IsOk());
}

KB_TEST("core: Recover refuses with Fault after max_attempts failed attempts; a plain start clears "
        "it") {
  Script s{};
  h::HandlerConfig config = Config();
  config.recovery.backoff = Milliseconds{1U};
  config.recovery.max_attempts = 2U;
  Toy<kbt::PlanSync> t{s, config};
  const kb::Instant t0 = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), t0).IsOk());
  s.stages[0].fail = Status::NoDevice;
  s.stages[0].fail_times = kbt::kAlways;
  KB_CHECK(t.Recover(Key(), t0).IsOk());
  KB_CHECK(!t.StartStep(Key(), t0).IsOk());
  KB_CHECK(t.Recover(Key(), Plus(t0, 1000U)).IsOk());
  KB_CHECK(!t.StartStep(Key(), Plus(t0, 1000U)).IsOk());
  KB_CHECK_EQ(t.Recover(Key(), Plus(t0, 1000000U)).Error(), Status::Fault);
  KB_CHECK_EQ(t.Recover(Key(), Plus(t0, 100000000U)).Error(), Status::Fault);
  KB_CHECK_EQ(HealthOf(t).recovery_failures, 2U);
  // A start that is not a recovery (the start failed, no driver is left) succeeds and resets the
  // attempts.
  // cppcheck-suppress [redundantAssignment,unreadVariable]
  s.stages[0].fail_times = 0U;
  const kb::Result<h::StartProgress> r = t.StartStep(Key(), Plus(t0, 2000U));
  KB_CHECK(r.IsOk() && (r.Value() == h::StartProgress::Done));
  KB_CHECK_EQ(HealthOf(t).recoveries, 0U);
  KB_CHECK(t.Recover(Key(), Plus(t0, 2000U)).IsOk());
}

KB_TEST("core: Recover during a running start begins a recovery run") {
  Script s{};
  s.stages[0].pending = 3U;
  Toy<kbt::PlanPolled> t{s, Config()};
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(t.StartStep(Key(), t0).IsOk());
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Starting);
  KB_CHECK(t.Recover(Key(), t0).IsOk());
  KB_CHECK_EQ(s.destroyed, 1U);
  KB_CHECK(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(ReportOf(t).runs, 2U);
  KB_CHECK_EQ(HealthOf(t).recoveries, 1U);
}

KB_TEST("core: a driver already in place when a run begins is kept") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
  KB_REQUIRE(t.Recover(Key(), kb::rtos::Now()).IsOk());
  KB_REQUIRE(s.stash != nullptr);
  s.stash->emplace(s); // the binding built its driver ahead of the run
  KB_CHECK_EQ(s.emplaced, 2U);
  KB_CHECK(t.StartStep(Key(), kb::rtos::Now()).IsOk());
  KB_CHECK_EQ(s.emplace_calls, 1U);
  KB_CHECK_EQ(s.emplaced, 2U);
}

// --- served calls ------------------------------------------------------------------------------

KB_TEST("core: Execute runs the command and records health: Degraded below the threshold, then "
        "Faulted") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()}; // fault_after 3
  const kb::Instant now = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), now).IsOk());
  const kb::Result<kbt::Reply> ok = t.Execute(Key(), Command{.op = 4U, .arg = 41U}, now);
  KB_REQUIRE(ok.IsOk());
  KB_CHECK_EQ(ok.Value().value, 42U);
  KB_CHECK_EQ(s.last_command.op, 4U);
  KB_CHECK_EQ(HealthOf(t).ok_calls, 2U);

  s.run_status = Status::BusError;
  // cppcheck-suppress unreadVariable
  s.run_detail = 17U;
  const kb::Result<kbt::Reply> bad = t.Execute(Key(), Command{}, Plus(now, 10U));
  KB_CHECK_EQ(bad.Error(), Status::BusError);
  KB_CHECK_EQ(bad.Detail(), 17U);
  KB_CHECK(!t.Execute(Key(), Command{}, now).IsOk());
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Degraded);
  KB_CHECK_EQ(health.consecutive, 2U);
  KB_CHECK_EQ(health.last_detail, 17U);
  KB_CHECK_EQ(health.last_error_us, now.Us());
  // cppcheck-suppress redundantAssignment
  s.run_status = Status::Ok;
  KB_CHECK(t.Execute(Key(), Command{}, now).IsOk());
  health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  KB_CHECK_EQ(health.consecutive, 0U);
  // cppcheck-suppress [redundantAssignment,unreadVariable]
  s.run_status = Status::BusError;
  for (uint32_t i = 0U; i < 3U; ++i) {
    KB_CHECK(!t.Execute(Key(), Command{}, now).IsOk());
  }
  health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.failed_calls, 5U);
  const uint32_t runs = s.runs;
  KB_CHECK_EQ(t.Execute(Key(), Command{}, now).Error(), Status::NotReady);
  KB_CHECK_EQ(s.runs, runs);
}

KB_TEST("core: IsFatal faults at once; without it the same failure only degrades") {
  {
    Script s{};
    s.fatal_status = Status::CrcMismatch;
    Toy<kbt::PlanSync> t{s, Config()};
    KB_REQUIRE(t.StartStep(Key(), kb::rtos::Now()).IsOk());
    // cppcheck-suppress unreadVariable
    s.run_status = Status::CrcMismatch;
    KB_CHECK(!t.Execute(Key(), Command{}, kb::rtos::Now()).IsOk());
    KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Faulted);
  }
  {
    Script s{};
    s.fatal_status = Status::CrcMismatch;
    Toy<kbt::PlanSync, feat::kAll & ~feat::kFatal> t{s, Config()};
    KB_REQUIRE(t.StartStep(Key(), kb::rtos::Now()).IsOk());
    // cppcheck-suppress unreadVariable
    s.run_status = Status::CrcMismatch;
    KB_CHECK(!t.Execute(Key(), Command{}, kb::rtos::Now()).IsOk());
    KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Degraded);
  }
}

KB_TEST("core: by default InvalidArgument is refused, not counted against health") {
  Script s{};
  Toy<kbt::PlanSync, feat::kAll & ~feat::kCounts> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), now).IsOk());
  s.run_status = Status::InvalidArgument;
  // cppcheck-suppress unreadVariable
  s.run_detail = 5U;
  KB_CHECK_EQ(t.Execute(Key(), Command{}, now).Error(), Status::InvalidArgument);
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  KB_CHECK_EQ(health.refused_calls, 1U);
  KB_CHECK_EQ(health.failed_calls, 0U);
  KB_CHECK_EQ(health.consecutive, 0U);
  KB_CHECK_EQ(health.last_status, Status::InvalidArgument);
  KB_CHECK_EQ(health.last_detail, 5U);
  KB_CHECK(health.has_error);
  // cppcheck-suppress [redundantAssignment,unreadVariable]
  s.run_status = Status::OutOfRange;
  KB_CHECK(!t.Execute(Key(), Command{}, now).IsOk());
  health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Degraded);
  KB_CHECK_EQ(health.failed_calls, 1U);
}

KB_TEST("core: CountsAgainstHealth overrides which failures are the caller's mistake") {
  Script s{};
  s.refused_status = Status::OutOfRange;
  Toy<kbt::PlanSync> t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  KB_REQUIRE(t.StartStep(Key(), now).IsOk());
  s.run_status = Status::OutOfRange;
  KB_CHECK(!t.Execute(Key(), Command{}, now).IsOk());
  h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.refused_calls, 1U);
  KB_CHECK_EQ(health.state, h::HealthState::Ready);
  // cppcheck-suppress [redundantAssignment,unreadVariable]
  s.run_status = Status::InvalidArgument; // counted for this binding
  KB_CHECK(!t.Execute(Key(), Command{}, now).IsOk());
  health = HealthOf(t);
  KB_CHECK_EQ(health.refused_calls, 1U);
  KB_CHECK_EQ(health.state, h::HealthState::Degraded);
}

KB_TEST("core: Sample publishes the snapshot with its sequence and age; a failed sample keeps it") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
  KB_CHECK_EQ(t.Latest().Error(), Status::NotReady); // started, but nothing sampled yet
  s.measure_value = 7U;
  const kb::Instant t0 = kb::rtos::Now();
  const kb::Result<Reading> first = t.Sample(Key(), t0);
  KB_REQUIRE(first.IsOk());
  KB_CHECK_EQ(first.Value().value, 7U);
  kb::Result<h::Snapshot<Reading>> latest = t.Latest();
  KB_REQUIRE(latest.IsOk());
  KB_CHECK_EQ(latest.Value().value.value, 7U);
  KB_CHECK_EQ(latest.Value().sequence, 1U);
  KB_CHECK_EQ(latest.Value().age_us, 0U);
  kb::rtos::none::AdvanceTicks(4U);
  latest = t.Latest();
  KB_REQUIRE(latest.IsOk());
  KB_CHECK_EQ(latest.Value().age_us, 4000U);

  // cppcheck-suppress [redundantAssignment,unreadVariable]
  s.measure_value = 8U;
  KB_CHECK(t.Sample(Key(), kb::rtos::Now()).IsOk());
  latest = t.Latest();
  KB_CHECK(latest.IsOk() && (latest.Value().sequence == 2U) && (latest.Value().value.value == 8U));

  // cppcheck-suppress unreadVariable
  s.measure_status = Status::Timeout;
  // cppcheck-suppress unreadVariable
  s.measure_detail = 2U;
  const kb::Result<Reading> failed = t.Sample(Key(), kb::rtos::Now());
  KB_CHECK_EQ(failed.Error(), Status::Timeout);
  KB_CHECK_EQ(failed.Detail(), 2U);
  latest = t.Latest();
  KB_CHECK(latest.IsOk() && (latest.Value().sequence == 2U) && (latest.Value().value.value == 8U));
  const h::HealthRecord health = HealthOf(t);
  KB_CHECK_EQ(health.state, h::HealthState::Degraded);
  KB_CHECK_EQ(health.last_status, Status::Timeout);
  KB_CHECK_EQ(s.measures, 3U);
}

KB_TEST("core: without Run or Measure the calls are Unsupported") {
  Script s{};
  using Plain = Toy<kbt::PlanSync, feat::kAll & ~(feat::kMeasure | feat::kRun)>;
  static_assert(!Plain::Measures());
  Plain t{s, Config()};
  const kb::Instant now = kb::rtos::Now();
  KB_CHECK_EQ(t.Sample(Key(), now).Error(), Status::Unsupported); // even before a start
  KB_REQUIRE(t.StartStep(Key(), now).IsOk());
  h::HealthRecord before = HealthOf(t);
  KB_CHECK_EQ(t.Sample(Key(), now).Error(), Status::Unsupported);
  h::HealthRecord after = HealthOf(t);
  KB_CHECK_EQ(after.ok_calls, before.ok_calls); // a Sample without Measure is not recorded
  KB_CHECK_EQ(after.failed_calls, before.failed_calls);
  KB_CHECK_EQ(t.Latest().Error(), Status::NotReady);

  KB_CHECK_EQ(t.Execute(Key(), Command{}, now).Error(), Status::Unsupported);
  after = HealthOf(t);
  KB_CHECK_EQ(after.failed_calls, before.failed_calls); // an Execute without Run is not recorded
  KB_CHECK_EQ(after.state, h::HealthState::Ready);
}

KB_TEST("core: Beat feeds the liveness beat; Health and Startup are copies") {
  Script s{};
  Toy<kbt::PlanSync> t{s, Config()};
  KB_CHECK_EQ(t.Liveness().Beats(), 0U);
  const kb::Instant now = Plus(kb::rtos::Now(), 123U);
  t.Beat(Key(), now);
  t.Beat(Key(), now);
  KB_CHECK_EQ(t.Liveness().Beats(), 2U);
  KB_CHECK(t.Liveness().LastBeat() == now);
  KB_REQUIRE(t.EnsureInitialized(Key()).IsOk());
  h::HealthRecord copy = HealthOf(t);
  copy.state = h::HealthState::Faulted; // a copy: the handler is unchanged
  KB_CHECK_EQ(HealthOf(t).state, h::HealthState::Ready);
  KB_CHECK_EQ(copy.state, h::HealthState::Faulted);
  h::StartupReport report = ReportOf(t);
  report.runs = 99U;
  KB_CHECK_EQ(ReportOf(t).runs, 1U);
  KB_CHECK_EQ(report.runs, 99U);
}
