// The bus domains and both access facades on the none backend (one context, fake time): the owner
// service pass and its violation check, bus statistics and recovery, BusHold, the guarded facade's
// API and its lock timeouts, and the owner facade's single-context logic (start steps, sample
// period, request slots); a fake-time timer plays the owner task where a caller waits. The multi-
// threaded behaviour is in test_threads.cpp.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
// cppcheck reads the test scripts' fields as unused: the handler under test reads them through a
// reference. cppcheck-suppress-file unreadVariable cppcheck-suppress-file redundantAssignment
#include <array>
#include <cstddef>
#include <cstdint>
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
namespace host = kb::iface::host;

namespace {

using Bus = host::FakeI2cBus;
using OwnerDomain = h::BusDomain<Bus, h::ThreadModel::Owner>;
using GuardedDomain = h::BusDomain<Bus, h::ThreadModel::Guarded>;
using RecoverableGuarded = h::BusDomain<kbt::RecoverableI2c, h::ThreadModel::Guarded>;
using RecoverableOwner = h::BusDomain<kbt::RecoverableI2c, h::ThreadModel::Owner>;

// The bus kind checks.
static_assert(h::TaggedBus<h::BusTag<h::BusKind::Spi>, h::BusKind::Spi>);
static_assert(!h::TaggedBus<h::BusTag<h::BusKind::Spi>, h::BusKind::I2c>);
static_assert(h::BusOfKind<h::BusTag<h::BusKind::Can>, h::BusKind::Can>);
static_assert(!h::BusOfKind<h::BusTag<h::BusKind::Can>, h::BusKind::Uart>);
static_assert(h::BusOfKind<host::FakeI2cBus, h::BusKind::I2c>);
static_assert(!h::BusOfKind<host::FakeI2cBus, h::BusKind::Spi>);
static_assert(h::BusOfKind<host::FakeSpiDevice, h::BusKind::Spi>);
static_assert(h::BusOfKind<host::FakeUart, h::BusKind::Uart>);
static_assert(h::BusOfKind<host::FakeAdc, h::BusKind::Adc>);
static_assert(h::BusOfKind<host::FakeCanBus, h::BusKind::Can>);
static_assert(!h::BusOfKind<host::FakeCanBus, h::BusKind::Adc>);
static_assert(
    h::BusOfKind<host::FakePwm, h::BusKind::Pio>); // any bus type drives a pulse peripheral
static_assert(h::BusOfKind<kbt::RecoverableI2c, h::BusKind::I2c>);
static_assert(h::RecoverableBus<kbt::RecoverableI2c>);
static_assert(!h::RecoverableBus<host::FakeI2cBus>);
static_assert(h::Handler<Toy<kbt::PlanSync>>);
static_assert(h::HandlerOnDomain<Toy<kbt::PlanSync>, GuardedDomain>);
static_assert(!h::HandlerOnDomain<Toy<kbt::PlanSync>,
                                  h::BusDomain<host::FakeSpiDevice, h::ThreadModel::Owner>>);
static_assert(OwnerDomain::kModel == h::ThreadModel::Owner);
static_assert(GuardedDomain::kModel == h::ThreadModel::Guarded);

h::HandlerConfig Config() noexcept {
  return h::HandlerConfig{};
}

template <typename F>
h::HealthRecord HealthOf(const F& facade) noexcept {
  const kb::Result<h::HealthRecord> r = facade.Health();
  return r.IsOk() ? r.Value() : h::HealthRecord{};
}

/// An access the owner pass services: logs its order.
struct Probe {
  std::array<uint8_t, 8>* log{nullptr};
  std::size_t* logged{nullptr};
  uint8_t id{0U};
  kb::Instant seen{0U};

  [[nodiscard]] bool Service(const h::ServiceKey& key, kb::Instant now) noexcept {
    static_cast<void>(key);
    seen = now;
    if ((log != nullptr) && (logged != nullptr) && (*logged < log->size())) {
      (*log)[*logged] = id;
      ++(*logged);
    }
    return true;
  }
};

/// An access that logs like a Probe, then may start a nested pass over `inner` from inside the
/// pass that services it. The inner access is a plain Probe, so the nested pass is a different
/// function and no call chain leads back here.
struct NestingProbe {
  Probe probe;
  OwnerDomain* nest{nullptr};
  Probe* inner{nullptr};
  Status nested{Status::Ok};

  [[nodiscard]] bool Service(const h::ServiceKey& key, kb::Instant now) noexcept {
    const bool ok = probe.Service(key, now);
    if ((nest != nullptr) && (inner != nullptr)) {
      nested = nest->Service(now, *inner).Error();
    }
    return ok;
  }
};

/// Runs one owner pass from the fake-time timer.
template <typename Domain, typename Access>
struct Pump {
  Domain* domain{nullptr};
  Access* access{nullptr};
  uint32_t passes{0U};
  static void Run(void* context) noexcept {
    auto* self = static_cast<Pump*>(context);
    if (self->domain->Service(kb::rtos::Now(), *self->access).IsOk()) {
      ++self->passes;
    }
  }
};

} // namespace

// --- Remaining ---------------------------------------------------------------------------------

KB_TEST("facade: Remaining gives whole milliseconds left, 0 once spent") {
  uint64_t elapsed = 3500U;
  KB_CHECK(h::Remaining(Milliseconds{10U}, elapsed) == Milliseconds{7U});
  elapsed = 0U;
  KB_CHECK(h::Remaining(Milliseconds{10U}, elapsed) == Milliseconds{10U});
  elapsed = 10000U;
  KB_CHECK(h::Remaining(Milliseconds{10U}, elapsed) == Milliseconds{0U});
  elapsed = 99000000U;
  KB_CHECK(h::Remaining(Milliseconds{10U}, elapsed) == Milliseconds{0U});
  static_assert(h::Remaining(Milliseconds{0U}, 0U) == Milliseconds{0U});
}

// --- owner bus domain --------------------------------------------------------------------------

KB_TEST("owner domain: one pass services every access in order and counts the pass") {
  Bus bus{};
  OwnerDomain domain{bus};
  KB_CHECK(&domain.Get() == &bus);
  std::array<uint8_t, 8> log{};
  std::size_t logged = 0U;
  Probe a{.log = &log, .logged = &logged, .id = 1U};
  Probe b{.log = &log, .logged = &logged, .id = 2U};
  Probe c{.log = &log, .logged = &logged, .id = 3U};
  const kb::Instant now = Plus(kb::rtos::Now(), 77U);
  KB_CHECK(domain.Service(now, a, b, c).IsOk());
  KB_CHECK_EQ(logged, 3U);
  KB_CHECK_EQ(log[0], 1U);
  KB_CHECK_EQ(log[1], 2U);
  KB_CHECK_EQ(log[2], 3U);
  KB_CHECK(b.seen == now);
  KB_CHECK(domain.Service(now, b).IsOk());
  const h::OwnerStats st = domain.Stats();
  KB_CHECK_EQ(st.passes, 2U);
  KB_CHECK_EQ(st.violations, 0U);
}

KB_TEST("owner domain: a second pass while one runs is refused Busy and counted") {
  Bus bus{};
  OwnerDomain domain{bus};
  std::array<uint8_t, 8> log{};
  std::size_t logged = 0U;
  Probe inner{.log = &log, .logged = &logged, .id = 9U};
  NestingProbe outer{.probe = {.log = &log, .logged = &logged, .id = 1U}};
  outer.nest = &domain;
  outer.inner = &inner;
  KB_CHECK(domain.Service(kb::rtos::Now(), outer).IsOk());
  KB_CHECK_EQ(outer.nested, Status::Busy);
  KB_CHECK_EQ(logged, 1U); // the nested pass serviced nothing
  h::OwnerStats st = domain.Stats();
  KB_CHECK_EQ(st.passes, 1U);
  KB_CHECK_EQ(st.violations, 1U);
  outer.nest = nullptr;
  KB_CHECK(domain.Service(kb::rtos::Now(), outer, inner).IsOk()); // the domain is free again
  st = domain.Stats();
  KB_CHECK_EQ(st.passes, 2U);
  KB_CHECK_EQ(logged, 3U);
}

KB_TEST("owner domain: RecoverBus where the bus supports it, Unsupported otherwise") {
  kbt::RecoverableI2c rbus{};
  RecoverableOwner rdomain{rbus};
  KB_CHECK(rdomain.RecoverBus().IsOk());
  // cppcheck-suppress unreadVariable
  rbus.answer = Status::BusError;
  KB_CHECK_EQ(rdomain.RecoverBus().Error(), Status::BusError);
  KB_CHECK_EQ(rbus.calls, 2U);
  Bus bus{};
  OwnerDomain domain{bus};
  KB_CHECK_EQ(domain.RecoverBus().Error(), Status::Unsupported);
}

KB_TEST("owner domain: a hold excludes another hold and the service pass; none waits") {
  Bus bus{};
  OwnerDomain domain{bus};
  {
    const h::BusHold<OwnerDomain> a = domain.Hold(Milliseconds{0U});
    const h::BusHold<OwnerDomain> b = domain.Hold(Milliseconds{0U});
    KB_CHECK(a.OwnsLock());
    KB_CHECK(!b.OwnsLock());
    KB_CHECK_EQ(domain.Service(kb::rtos::Now()).Error(), Status::Busy);
  }
  const h::OwnerStats st = domain.Stats();
  KB_CHECK_EQ(st.holds, 1U);
  KB_CHECK_EQ(st.violations, 2U);
  KB_CHECK(domain.Service(kb::rtos::Now()).IsOk()); // released with the hold
}

// --- guarded bus domain ------------------------------------------------------------------------

KB_TEST("guarded domain: Create once; holds are counted and the longest hold is kept") {
  Bus bus{};
  GuardedDomain domain{bus};
  KB_CHECK(&domain.Get() == &bus);
  {
    const h::BusHold<GuardedDomain> before = domain.Hold(Milliseconds{1U});
    KB_CHECK(!before.OwnsLock()); // no mutex yet
  }
  KB_CHECK(domain.Create().IsOk());
  KB_CHECK_EQ(domain.Create().Error(), Status::AlreadyStarted);
  {
    const h::BusHold<GuardedDomain> hold = domain.Hold(Milliseconds{1U});
    KB_CHECK(hold.OwnsLock());
    kb::rtos::none::AdvanceTicks(3U);
  }
  {
    const h::BusHold<GuardedDomain> hold = domain.Hold(Milliseconds{1U});
    KB_CHECK(hold.OwnsLock());
    kb::rtos::none::AdvanceTicks(1U);
  }
  h::BusStats st = domain.Stats();
  KB_CHECK_EQ(st.holds, 2U);
  KB_CHECK_EQ(st.max_hold_us, 3000U);
  KB_CHECK_EQ(st.lock_timeouts, 1U);
  KB_CHECK_EQ(st.recoveries, 0U);
  {
    // cppcheck-suppress unreadVariable
    const h::BusHold<GuardedDomain> hold = domain.Hold(Milliseconds{1U});
    const kb::Instant now = kb::rtos::Now();
    kb::rtos::none::SetTicks((now.Us() / 1000U) + 5000000U); // a hold of 5000 s
  }
  st = domain.Stats();
  KB_CHECK_EQ(st.max_hold_us, std::numeric_limits<uint32_t>::max());
}

KB_TEST("guarded domain: a second holder times out within its bound") {
  Bus bus{};
  GuardedDomain domain{bus};
  KB_REQUIRE(domain.Create().IsOk());
  const h::BusHold<GuardedDomain> first = domain.Hold(Milliseconds{1U});
  KB_REQUIRE(first.OwnsLock());
  const kb::Instant before = kb::rtos::Now();
  const h::BusHold<GuardedDomain> second = domain.Hold(Milliseconds{4U});
  KB_CHECK(!second.OwnsLock());
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 4000U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 1U);
  KB_CHECK_EQ(domain.Stats().holds, 1U);
}

KB_TEST("guarded domain: RecoverBus under the bus lock, Unsupported, failed, or Timeout") {
  Bus bus{};
  GuardedDomain plain{bus};
  KB_REQUIRE(plain.Create().IsOk());
  KB_CHECK_EQ(plain.RecoverBus(Milliseconds{1U}).Error(), Status::Unsupported);

  kbt::RecoverableI2c rbus{};
  RecoverableGuarded domain{rbus};
  KB_CHECK_EQ(domain.RecoverBus(Milliseconds{1U}).Error(), Status::Timeout); // no mutex yet
  KB_REQUIRE(domain.Create().IsOk());
  KB_CHECK(domain.RecoverBus(Milliseconds{1U}).IsOk());
  KB_CHECK_EQ(domain.Stats().recoveries, 1U);
  rbus.answer = Status::BusError;
  KB_CHECK_EQ(domain.RecoverBus(Milliseconds{1U}).Error(), Status::BusError);
  KB_CHECK_EQ(domain.Stats().recoveries, 1U);
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  rbus.answer = Status::Ok;
  {
    const h::BusHold<RecoverableGuarded> hold = domain.Hold(Milliseconds{1U});
    KB_REQUIRE(hold.OwnsLock());
    KB_CHECK_EQ(domain.RecoverBus(Milliseconds{2U}).Error(), Status::Timeout);
  }
  KB_CHECK_EQ(rbus.calls, 2U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 2U);
}

// --- Guarded -----------------------------------------------------------------------------------

namespace {
using GToy = Toy<kbt::PlanSync>;
using G = h::Guarded<GToy, GuardedDomain>;
} // namespace

KB_TEST("guarded: the caller API runs each call under both locks") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  s.measure_value = 21U;
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(g.Create().IsOk());
  KB_CHECK_EQ(g.Create().Error(), Status::AlreadyStarted);
  KB_CHECK_EQ(g.Request(Command{}, Milliseconds{5U}).Error(), Status::NotReady); // not started
  KB_CHECK(g.EnsureStarted(Milliseconds{5U}).IsOk());
  KB_CHECK_EQ(HealthOf(g).state, h::HealthState::Ready);
  KB_CHECK(g.EnsureStarted(Milliseconds{5U}).IsOk()); // idempotent
  KB_CHECK_EQ(s.stages[0].calls, 1U);
  const kb::Result<kbt::Reply> reply = g.Request(Command{.op = 2U, .arg = 5U}, Milliseconds{5U});
  KB_REQUIRE(reply.IsOk());
  KB_CHECK_EQ(reply.Value().value, 6U);
  KB_CHECK_EQ(s.last_command.op, 2U);

  const kb::Result<h::Snapshot<Reading>> sampled = g.Sample(Milliseconds{5U});
  KB_REQUIRE(sampled.IsOk());
  KB_CHECK_EQ(sampled.Value().value.value, 21U);
  KB_CHECK_EQ(sampled.Value().sequence, 1U);
  // cppcheck-suppress unreadVariable
  s.measure_status = Status::Timeout;
  // cppcheck-suppress unreadVariable
  s.measure_detail = 3U;
  const kb::Result<h::Snapshot<Reading>> failed = g.Sample(Milliseconds{5U});
  KB_CHECK_EQ(failed.Error(), Status::Timeout);
  KB_CHECK_EQ(failed.Detail(), 3U);
  const kb::Result<h::Snapshot<Reading>> latest = g.Latest();
  KB_CHECK(latest.IsOk() && (latest.Value().sequence == 1U));

  KB_CHECK(g.RequestRecovery(Milliseconds{5U}).IsOk());
  KB_CHECK_EQ(HealthOf(g).recoveries, 1U);
  KB_CHECK_EQ(s.recovers, 1U);
  const kb::Result<h::StartupReport> report = g.Startup();
  KB_CHECK(report.IsOk() && (report.Value().runs == 2U));
  KB_CHECK(g.Stop(Milliseconds{5U}).IsOk());
  KB_CHECK_EQ(HealthOf(g).state, h::HealthState::Unknown);
  KB_CHECK_EQ(g.RequestRecovery(Milliseconds{5U}).Error(), Status::NotReady);
  KB_CHECK_EQ(s.stages[0].calls, 2U); // no start after the refused recovery
  // Every call that got both locks beat once; the refused one too (it ran).
  KB_CHECK_EQ(g.Liveness().Beats(), 9U);
  KB_CHECK_EQ(g.LockTimeouts(), 0U);
  KB_CHECK_EQ(domain.Stats().holds, 9U);
}

KB_TEST("guarded: a failed start returns its status; a later call starts again") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  s.stages[0].fail = Status::NoDevice;
  s.stages[0].fail_times = 1U;
  s.stages[0].detail = 4U;
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  const kb::Result<void> first = g.EnsureStarted(Milliseconds{5U});
  KB_CHECK_EQ(first.Error(), Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), 4U);
  KB_CHECK(g.EnsureStarted(Milliseconds{5U}).IsOk());
}

KB_TEST("guarded: a polled start runs to the end under the locks") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  s.stages[0].pending = 3U;
  h::Guarded<Toy<kbt::PlanPolled>, GuardedDomain> g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_CHECK(g.EnsureStarted(Milliseconds{1U}).IsOk());
  KB_CHECK_EQ(s.stages[0].calls, 4U);
  KB_CHECK_EQ(domain.Stats().holds, 1U);
  KB_CHECK(domain.Stats().max_hold_us >= 6000U);
}

KB_TEST("guarded: without the handler mutex every call is Timeout and sends nothing") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_CHECK_EQ(g.EnsureStarted(Milliseconds{2U}).Error(), Status::Timeout);
  KB_CHECK_EQ(g.Request(Command{}, Milliseconds{2U}).Error(), Status::Timeout);
  KB_CHECK_EQ(g.Sample(Milliseconds{2U}).Error(), Status::Timeout);
  KB_CHECK_EQ(g.Stop(Milliseconds{2U}).Error(), Status::Timeout);
  KB_CHECK_EQ(g.LockTimeouts(), 4U);
  KB_CHECK_EQ(s.emplaced, 0U);
  KB_CHECK_EQ(g.Liveness().Beats(), 0U);
  KB_CHECK_EQ(domain.Stats().holds, 0U);
}

KB_TEST("guarded: the handler lock is bounded: a call made while it is held times out") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(Milliseconds{5U}).IsOk());
  const kb::Instant before = kb::rtos::Now();
  // The operation runs under both locks and calls the facade again: the handler lock is taken.
  const kb::Result<kbt::Reply> nested =
      g.Call(Milliseconds{20U}, [&g](GToy& handler, const h::ServiceKey& key) noexcept {
        static_cast<void>(handler);
        static_cast<void>(key);
        return g.Request(Command{}, Milliseconds{3U});
      });
  KB_CHECK_EQ(nested.Error(), Status::Timeout);
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 3000U);
  KB_CHECK_EQ(g.LockTimeouts(), 1U);
  KB_CHECK_EQ(s.runs, 0U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 0U); // it never reached the bus lock
}

KB_TEST("guarded: the bus lock is bounded: a call while another holder has the bus times out") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(Milliseconds{5U}).IsOk());
  {
    const h::BusHold<GuardedDomain> other = domain.Hold(Milliseconds{1U});
    KB_REQUIRE(other.OwnsLock());
    const kb::Instant before = kb::rtos::Now();
    KB_CHECK_EQ(g.Request(Command{}, Milliseconds{5U}).Error(), Status::Timeout);
    KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 5000U);
    KB_CHECK_EQ(g.RequestRecovery(Milliseconds{0U}).Error(), Status::Timeout);
  }
  KB_CHECK_EQ(s.runs, 0U);
  KB_CHECK_EQ(s.recovers, 0U);
  KB_CHECK_EQ(g.LockTimeouts(), 2U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 2U);
  KB_CHECK(g.Request(Command{}, Milliseconds{5U}).IsOk()); // the bus is free again
}

KB_TEST("guarded: Call runs any operation that returns a kb::Result") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  const kb::Result<uint32_t> value = g.Call(
      Milliseconds{5U},
      [](GToy& handler, const h::ServiceKey& key, uint32_t add) noexcept {
        static_cast<void>(key);
        return kb::Result<uint32_t>::Success(handler.Settings().health.fault_after + add);
      },
      4U);
  KB_REQUIRE(value.IsOk());
  KB_CHECK_EQ(value.Value(), 7U);
}

// --- OwnerAccess: one context ------------------------------------------------------------------

namespace {
using OToy = Toy<kbt::PlanPolled>;
using OA = h::OwnerAccess<OToy, OwnerDomain, 2U>;
using OPump = Pump<OwnerDomain, OA>;
} // namespace

KB_TEST("owner: a pass with no request and no start only beats") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  KB_CHECK(&oa.BusDomainOf() == &domain);
  KB_CHECK(domain.Service(kb::rtos::Now(), oa).IsOk());
  KB_CHECK_EQ(oa.Liveness().Beats(), 1U);
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Unknown);
  KB_CHECK_EQ(s.stages[0].calls, 0U);
  KB_CHECK_EQ(s.measures, 0U);
  const h::ChannelStats st = oa.Requests();
  KB_CHECK_EQ(st.posted + st.busy + st.timeouts + st.completed + st.skipped, 0U);
}

KB_TEST(
    "owner: RequestStart runs one start step per pass; the periodic read then follows its period") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].pending = 2U;
  s.measure_value = 5U;
  h::HandlerConfig config = Config();
  config.sample_period = Milliseconds{10U};
  OA oa{domain, s, config};
  KB_REQUIRE(oa.Create().IsOk());
  oa.RequestStart();
  // The pass times start at 0, so the first read (at 3 ms) is due only because nothing was read
  // yet, not because 10 ms passed since the clock's start.
  const kb::Instant t0{0U};
  KB_CHECK(domain.Service(t0, oa).IsOk());
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Starting);
  KB_CHECK(domain.Service(Plus(t0, 1000U), oa).IsOk());
  KB_CHECK_EQ(s.stages[0].calls, 2U);
  KB_CHECK(domain.Service(Plus(t0, 2000U), oa).IsOk());
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Ready);
  KB_CHECK_EQ(s.measures, 0U); // the pass that ended the start did not sample
  KB_CHECK(domain.Service(Plus(t0, 3000U), oa).IsOk());
  KB_CHECK_EQ(s.measures, 1U); // the first read is due at once
  const kb::Result<h::Snapshot<Reading>> latest = oa.Latest();
  KB_CHECK(latest.IsOk() && (latest.Value().value.value == 5U));
  KB_CHECK(domain.Service(Plus(t0, 12999U), oa).IsOk());
  KB_CHECK_EQ(s.measures, 1U);
  KB_CHECK(domain.Service(Plus(t0, 13000U), oa).IsOk());
  KB_CHECK_EQ(s.measures, 2U);
  KB_CHECK_EQ(oa.Liveness().Beats(), 6U);
  KB_CHECK_EQ(s.stages[0].calls, 3U); // started: no further start steps
  const kb::Result<h::StartupReport> report = oa.Startup();
  KB_CHECK(report.IsOk() && report.Value().complete);
}

KB_TEST(
    "owner: with sample_period 0 every serving pass samples; a faulted handler is not sampled") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()}; // fault_after 3
  KB_REQUIRE(oa.Create().IsOk());
  oa.RequestStart();
  const kb::Instant t0 = kb::rtos::Now();
  KB_CHECK(domain.Service(t0, oa).IsOk());
  KB_CHECK(domain.Service(t0, oa).IsOk());
  KB_CHECK(domain.Service(t0, oa).IsOk());
  KB_CHECK_EQ(s.measures, 2U);
  // cppcheck-suppress unreadVariable
  s.measure_status = Status::BusError;
  for (uint32_t i = 0U; i < 5U; ++i) {
    KB_CHECK(domain.Service(t0, oa).IsOk());
  }
  KB_CHECK_EQ(s.measures, 5U); // three failures fault it; then no read is due
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Faulted);
}

KB_TEST("owner: a handler without Measure is never sampled; a Sample request is Unsupported") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  using Plain = Toy<kbt::PlanSync, feat::kAll & ~feat::kMeasure>;
  using PA = h::OwnerAccess<Plain, OwnerDomain, 1U>;
  PA pa{domain, s, Config()};
  KB_REQUIRE(pa.Create().IsOk());
  pa.RequestStart();
  for (uint32_t i = 0U; i < 3U; ++i) {
    KB_CHECK(domain.Service(kb::rtos::Now(), pa).IsOk());
  }
  KB_CHECK_EQ(HealthOf(pa).state, h::HealthState::Ready);
  Pump<OwnerDomain, PA> pump{.domain = &domain, .access = &pa};
  const kbt::TickerScope ticker{&Pump<OwnerDomain, PA>::Run, &pump};
  KB_CHECK_EQ(pa.Sample(Milliseconds{5U}).Error(), Status::Unsupported);
  KB_CHECK(pump.passes >= 1U);
}

KB_TEST("owner: every caller request is completed by the serviced owner") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].pending = 2U;
  s.measure_value = 33U;
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  OPump pump{&domain, &oa};
  const kbt::TickerScope ticker{&OPump::Run, &pump};
  KB_CHECK(oa.EnsureStarted(Milliseconds{20U}).IsOk());
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Ready);
  KB_CHECK_EQ(s.stages[0].calls, 3U);
  KB_CHECK(oa.EnsureStarted(Milliseconds{20U}).IsOk()); // started: completes at once
  KB_CHECK_EQ(s.stages[0].calls, 3U);
  const kb::Result<kbt::Reply> reply = oa.Request(Command{.op = 1U, .arg = 9U}, Milliseconds{20U});
  KB_REQUIRE(reply.IsOk());
  KB_CHECK_EQ(reply.Value().value, 10U);
  const kb::Result<h::Snapshot<Reading>> sampled = oa.Sample(Milliseconds{20U});
  KB_REQUIRE(sampled.IsOk());
  KB_CHECK_EQ(sampled.Value().value.value, 33U);
  s.run_status = Status::Nack;
  KB_CHECK_EQ(oa.Request(Command{}, Milliseconds{20U}).Error(), Status::Nack);
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  s.run_status = Status::Ok;
  KB_CHECK(oa.RequestRecovery(Milliseconds{20U}).IsOk());
  KB_CHECK_EQ(HealthOf(oa).recoveries, 1U);
  KB_CHECK(oa.Stop(Milliseconds{20U}).IsOk());
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Unknown);
  KB_CHECK_EQ(oa.RequestRecovery(Milliseconds{20U}).Error(), Status::NotReady);
  KB_CHECK_EQ(oa.Sample(Milliseconds{20U}).Error(), Status::NotReady);
  const h::ChannelStats st = oa.Requests();
  KB_CHECK_EQ(st.posted, 9U);
  KB_CHECK_EQ(st.completed, 9U);
  KB_CHECK_EQ(st.timeouts, 0U);
}

KB_TEST("owner: a failed start completes its waiter with the failure") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].fail = Status::NoDevice;
  s.stages[0].fail_times = kbt::kAlways;
  s.stages[0].detail = 6U;
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  OPump pump{&domain, &oa};
  const kbt::TickerScope ticker{&OPump::Run, &pump};
  const kb::Result<void> r = oa.EnsureStarted(Milliseconds{10U});
  KB_CHECK_EQ(r.Error(), Status::NoDevice);
  KB_CHECK_EQ(r.Detail(), 6U);
  KB_CHECK_EQ(s.stages[0].calls, 1U); // the owner does not retry by itself
  const kb::Result<h::StartupReport> report = oa.Startup();
  KB_CHECK(report.IsOk() && (report.Value().status == Status::NoDevice));
}

KB_TEST("owner: RequestRecovery reports the recovery's failure, then the backoff") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  h::HandlerConfig config = Config();
  config.recovery.backoff = Milliseconds{1000U};
  OA oa{domain, s, config};
  KB_REQUIRE(oa.Create().IsOk());
  OPump pump{&domain, &oa};
  const kbt::TickerScope ticker{&OPump::Run, &pump};
  KB_REQUIRE(oa.EnsureStarted(Milliseconds{10U}).IsOk());
  s.stages[0].fail = Status::NoDevice;
  // cppcheck-suppress unreadVariable
  s.stages[0].fail_times = kbt::kAlways;
  KB_CHECK_EQ(oa.RequestRecovery(Milliseconds{10U}).Error(), Status::NoDevice);
  KB_CHECK_EQ(oa.RequestRecovery(Milliseconds{10U}).Error(), Status::Busy);
  KB_CHECK_EQ(HealthOf(oa).recovery_failures, 1U);
}

KB_TEST(
    "owner: without a service pass a caller times out; the abandoned request is skipped later") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  const kb::Instant before = kb::rtos::Now();
  KB_CHECK_EQ(oa.EnsureStarted(Milliseconds{5U}).Error(), Status::Timeout);
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 5000U);
  KB_CHECK_EQ(oa.Requests().timeouts, 1U);
  KB_CHECK(domain.Service(kb::rtos::Now(), oa).IsOk());
  KB_CHECK_EQ(oa.Requests().skipped, 1U);
  KB_CHECK_EQ(s.stages[0].calls, 0U); // the abandoned start was not run
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Unknown);
}

KB_TEST("owner: Busy when every request slot is in use") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()}; // two slots
  KB_REQUIRE(oa.Create().IsOk());
  KB_CHECK_EQ(oa.Request(Command{}, Milliseconds{1U}).Error(), Status::Timeout);
  KB_CHECK_EQ(oa.Stop(Milliseconds{1U}).Error(), Status::Timeout);
  // Both slots wait for the owner to free them.
  const kb::Instant before = kb::rtos::Now();
  KB_CHECK_EQ(oa.Request(Command{}, Milliseconds{50U}).Error(), Status::Busy);
  KB_CHECK_EQ(oa.EnsureStarted(Milliseconds{50U}).Error(), Status::Busy);
  KB_CHECK(kb::rtos::Now() == before); // Busy does not wait
  KB_CHECK_EQ(oa.Requests().busy, 2U);
  KB_CHECK(domain.Service(kb::rtos::Now(), oa).IsOk());
  KB_CHECK_EQ(oa.Requests().skipped, 2U);
  KB_CHECK_EQ(oa.Request(Command{}, Milliseconds{1U}).Error(),
              Status::Timeout); // a slot was free again
  KB_CHECK_EQ(s.runs, 0U);
}

namespace {

/// The owner pump of the Stop test: from pass `stop_at`, stops the handler from inside the pass
/// (the nested passes of the stop's own wait service the request).
struct StopPump {
  OwnerDomain* domain{nullptr};
  OA* access{nullptr};
  uint32_t passes{0U};
  uint32_t stop_at{3U};
  bool stopping{false};
  Status stop_result{Status::Fault};
  static void Run(void* context) noexcept {
    auto* self = static_cast<StopPump*>(context);
    if (self->stopping || (self->passes < self->stop_at)) {
      static_cast<void>(self->domain->Service(kb::rtos::Now(), *self->access));
      ++self->passes;
    } else if (self->passes == self->stop_at) {
      self->stopping = true;
      ++self->passes;
      self->stop_result = self->access->Stop(Milliseconds{3U}).Error();
      self->stopping = false;
    } else {
      static_cast<void>(self->domain->Service(kb::rtos::Now(), *self->access));
    }
  }
};

} // namespace

KB_TEST("owner: Stop completes the waiting start with NotReady") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].pending = kbt::kAlways; // the start never ends by itself
  s.stages[0].deadline_ms = 100000U;
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  StopPump pump{.domain = &domain, .access = &oa};
  const kbt::TickerScope ticker{&StopPump::Run, &pump};
  const kb::Result<void> start = oa.EnsureStarted(Milliseconds{20U});
  KB_CHECK_EQ(start.Error(), Status::NotReady);
  KB_CHECK_EQ(pump.stop_result, Status::Ok);
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Unknown);
  KB_CHECK(s.stages[0].calls >= 2U);
  KB_CHECK_EQ(s.destroyed, 1U);
}
