// BusHold and the held calls of both facades (ADR-0016), on the none backend (one context, fake
// time): Hold() on both domains, Bus(), Release(), moves, hold statistics; Guarded::Hold (both
// locks in order, refusals when either is not had, Timeout and NotReady, the bus and handler free
// again after release); OwnerAccess::Held (inline calls under the owner's hold, the pass refused
// meanwhile, a released hold or one of another domain object refused).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <type_traits>
#include <utility>

#include "handler_test_support.hpp"
#include "kb/rtos/none/control.hpp"
#include "kb/rtos/time.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
using kbt::Command;
using kbt::Script;
using kbt::Toy;
namespace h = kb::handlers;
namespace host = kb::iface::host;

namespace {

using Bus = host::FakeI2cBus;
using OwnerDomain = h::BusDomain<Bus, h::ThreadModel::Owner>;
using GuardedDomain = h::BusDomain<Bus, h::ThreadModel::Guarded>;
using GToy = Toy<kbt::PlanSync>;
using G = h::Guarded<GToy, GuardedDomain>;
using OA = h::OwnerAccess<GToy, OwnerDomain, 2U>;

static_assert(!std::is_copy_constructible_v<h::BusHold<GuardedDomain>>);
static_assert(std::is_nothrow_move_constructible_v<h::BusHold<GuardedDomain>>);
static_assert(!std::is_move_assignable_v<h::BusHold<OwnerDomain>>);
static_assert(!std::is_copy_constructible_v<h::GuardedHeld<G>> &&
              !std::is_move_constructible_v<h::GuardedHeld<G>>);
static_assert(!std::is_copy_constructible_v<h::OwnerHeld<OA>> &&
              !std::is_move_constructible_v<h::OwnerHeld<OA>>);

h::HandlerConfig Config() noexcept {
  return h::HandlerConfig{};
}

/// A guarded toy handler on its own guarded bus, created and started.
struct GuardedRig {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  GuardedRig() {
    static_cast<void>(domain.Create());
    static_cast<void>(g.Create());
    static_cast<void>(g.EnsureStarted(Milliseconds{5U}));
  }
};

/// An owner toy handler on its own owner bus, started by one service pass.
struct OwnerRig {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()};
  OwnerRig() {
    static_cast<void>(oa.Create());
    oa.RequestStart();
    static_cast<void>(domain.Service(kb::rtos::Now(), oa));
  }
};

} // namespace

// --- BusHold -----------------------------------------------------------------------------------

KB_TEST("bus hold: a guarded hold owns the bus, gives it, and releases it once") {
  Bus bus{};
  GuardedDomain domain{bus};
  KB_REQUIRE(domain.Create().IsOk());
  h::BusHold<GuardedDomain> hold = domain.Hold(Milliseconds{1U});
  KB_REQUIRE(hold.OwnsLock());
  KB_CHECK(hold.IsOn(domain));
  KB_CHECK(hold.Bus() == &bus);
  kb::rtos::none::AdvanceTicks(2U);
  hold.Release();
  KB_CHECK(!hold.OwnsLock());
  KB_CHECK(hold.Bus() == nullptr);
  hold.Release(); // nothing the second time: the next hold is had at once
  const h::BusHold<GuardedDomain> next = domain.Hold(Milliseconds{0U});
  KB_CHECK(next.OwnsLock());
  const h::BusStats st = domain.Stats();
  KB_CHECK_EQ(st.holds, 2U);
  KB_CHECK_EQ(st.max_hold_us, 2000U);
  KB_CHECK_EQ(st.lock_timeouts, 0U);
}

KB_TEST("bus hold: a hold that waits in vain is not counted as held; the longest hold is kept") {
  Bus bus{};
  GuardedDomain domain{bus};
  KB_REQUIRE(domain.Create().IsOk());
  {
    // cppcheck-suppress unreadVariable
    const h::BusHold<GuardedDomain> first = domain.Hold(Milliseconds{1U});
    h::BusHold<GuardedDomain> waited = domain.Hold(Milliseconds{7U}); // waits at least 7 ms
    KB_CHECK(!waited.OwnsLock());
    KB_CHECK(waited.Bus() == nullptr);
  }
  const uint32_t longest = domain.Stats().max_hold_us;
  KB_CHECK(longest >= 7000U); // the first hold lasted through the wait
  KB_CHECK_EQ(domain.Stats().holds, 1U);
  {
    // cppcheck-suppress unreadVariable
    const h::BusHold<GuardedDomain> quick = domain.Hold(Milliseconds{1U});
    kb::rtos::none::AdvanceTicks(1U);
  }
  KB_CHECK_EQ(domain.Stats().max_hold_us, longest);
}

KB_TEST("bus hold: a move hands the hold over; the moved-from hold owns and releases nothing") {
  Bus bus{};
  GuardedDomain domain{bus};
  KB_REQUIRE(domain.Create().IsOk());
  h::BusHold<GuardedDomain> a = domain.Hold(Milliseconds{1U});
  KB_REQUIRE(a.OwnsLock());
  {
    h::BusHold<GuardedDomain> b{std::move(a)};
    KB_CHECK(b.OwnsLock());
    KB_CHECK(b.Bus() == &bus);
    // The moved-from state is what this test checks.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    KB_CHECK(!a.OwnsLock());
    KB_CHECK(a.Bus() == nullptr);
    a.Release(); // must not unlock b's bus
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    KB_CHECK(!domain.Hold(Milliseconds{1U}).OwnsLock());
  }
  KB_CHECK(domain.Hold(Milliseconds{0U}).OwnsLock()); // b released it
}

KB_TEST("bus hold: no mutex yet: not owned, counted, nothing to release") {
  Bus bus{};
  GuardedDomain domain{bus};
  h::BusHold<GuardedDomain> hold = domain.Hold(Milliseconds{1U});
  KB_CHECK(!hold.OwnsLock());
  KB_CHECK(hold.IsOn(domain));
  hold.Release();
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 1U);
  KB_CHECK_EQ(domain.Stats().holds, 0U);
}

KB_TEST("bus hold: IsOn tells two domains of one type apart") {
  Bus bus_a{};
  Bus bus_b{};
  OwnerDomain a{bus_a};
  OwnerDomain b{bus_b};
  const h::BusHold<OwnerDomain> hold = a.Hold(Milliseconds{0U});
  KB_CHECK(hold.IsOn(a));
  KB_CHECK(!hold.IsOn(b));
}

KB_TEST("bus hold: an owner hold gives the bus and frees the pass flag on release and on move") {
  Bus bus{};
  OwnerDomain domain{bus};
  h::BusHold<OwnerDomain> hold = domain.Hold(Milliseconds{0U});
  KB_REQUIRE(hold.OwnsLock());
  KB_CHECK(hold.Bus() == &bus);
  h::BusHold<OwnerDomain> moved{std::move(hold)};
  KB_CHECK_EQ(domain.Service(kb::rtos::Now()).Error(), Status::Busy);
  moved.Release();
  KB_CHECK(domain.Service(kb::rtos::Now()).IsOk());
  KB_CHECK_EQ(domain.Stats().holds, 1U);
  KB_CHECK_EQ(domain.Stats().passes, 1U);
}

// --- Guarded::Hold -----------------------------------------------------------------------------

KB_TEST("guarded hold: calls run under both locks with no wait; others time out meanwhile") {
  GuardedRig r{};
  r.s.measure_value = 9U;
  {
    h::GuardedHeld<G> held = r.g.Hold(Milliseconds{5U});
    KB_REQUIRE(held.OwnsLock());
    const kb::Result<kbt::Reply> one = held.Request(Command{.op = 1U, .arg = 1U}, Milliseconds{0U});
    const kb::Result<kbt::Reply> two = held.Request(Command{.op = 1U, .arg = 2U}, Milliseconds{0U});
    KB_CHECK(one.IsOk() && (one.Value().value == 2U));
    KB_CHECK(two.IsOk() && (two.Value().value == 3U));
    const kb::Result<h::Snapshot<kbt::Reading>> sampled = held.Sample(Milliseconds{0U});
    KB_REQUIRE(sampled.IsOk());
    KB_CHECK_EQ(sampled.Value().value.value, 9U);
    KB_CHECK(held.Health().IsOk());
    KB_CHECK_EQ(r.g.Request(Command{}, Milliseconds{2U}).Error(), Status::Timeout);
    KB_CHECK(!r.domain.Hold(Milliseconds{2U}).OwnsLock());
  }
  KB_CHECK_EQ(r.s.runs, 2U);
  KB_CHECK_EQ(r.g.Liveness().Beats(), 4U); // the start, two requests and the sample
  KB_CHECK(r.g.Request(Command{}, Milliseconds{2U}).IsOk()); // both free again
}

KB_TEST("guarded hold: after Release() the hold refuses with NotReady and both locks are free") {
  GuardedRig r{};
  h::GuardedHeld<G> held = r.g.Hold(Milliseconds{5U});
  KB_REQUIRE(held.OwnsLock());
  held.Release();
  KB_CHECK(!held.OwnsLock());
  KB_CHECK_EQ(held.Request(Command{}, Milliseconds{1U}).Error(), Status::NotReady);
  KB_CHECK_EQ(held.Sample(Milliseconds{1U}).Error(), Status::NotReady);
  held.Release();
  KB_CHECK(r.g.Request(Command{}, Milliseconds{1U}).IsOk());
  KB_CHECK(r.domain.Hold(Milliseconds{0U}).OwnsLock());
  KB_CHECK_EQ(r.s.runs, 1U);
}

KB_TEST("guarded hold: a held bus fails the hold with Timeout and frees the handler lock") {
  GuardedRig r{};
  const uint32_t before = r.domain.Stats().lock_timeouts;
  {
    const h::BusHold<GuardedDomain> other = r.domain.Hold(Milliseconds{1U});
    KB_REQUIRE(other.OwnsLock());
    const kb::Instant t0 = kb::rtos::Now();
    h::GuardedHeld<G> held = r.g.Hold(Milliseconds{4U});
    KB_CHECK(!held.OwnsLock());
    KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), t0) >= 4000U); // waited its bound, no more
    KB_CHECK_EQ(held.Request(Command{}, Milliseconds{1U}).Error(), Status::Timeout);
    KB_CHECK_EQ(held.Sample(Milliseconds{1U}).Error(), Status::Timeout);
    // The handler lock was given back: a call reaches the bus lock (and times out there).
    KB_CHECK_EQ(r.g.Request(Command{}, Milliseconds{1U}).Error(), Status::Timeout);
  }
  KB_CHECK_EQ(r.domain.Stats().lock_timeouts, before + 2U);
  KB_CHECK_EQ(r.g.LockTimeouts(), 2U);
  KB_CHECK_EQ(r.s.runs, 0U);
}

KB_TEST("guarded hold: a held handler fails a second hold before it reaches the bus") {
  GuardedRig r{};
  h::GuardedHeld<G> first = r.g.Hold(Milliseconds{1U});
  KB_REQUIRE(first.OwnsLock());
  const uint32_t bus_timeouts = r.domain.Stats().lock_timeouts;
  h::GuardedHeld<G> second = r.g.Hold(Milliseconds{3U});
  KB_CHECK(!second.OwnsLock());
  KB_CHECK_EQ(second.Request(Command{}, Milliseconds{1U}).Error(), Status::Timeout);
  KB_CHECK_EQ(r.domain.Stats().lock_timeouts, bus_timeouts); // never waited for the bus
  KB_CHECK_EQ(r.g.LockTimeouts(), 1U);
  KB_CHECK(first.Request(Command{}, Milliseconds{0U}).IsOk());
}

KB_TEST("guarded hold: a sample that fails is forwarded with its detail") {
  GuardedRig r{};
  r.s.measure_status = Status::BusError;
  r.s.measure_detail = 12U;
  h::GuardedHeld<G> held = r.g.Hold(Milliseconds{1U});
  const kb::Result<h::Snapshot<kbt::Reading>> failed = held.Sample(Milliseconds{0U});
  KB_CHECK_EQ(failed.Error(), Status::BusError);
  KB_CHECK_EQ(failed.Detail(), 12U);
}

// --- OwnerAccess::Held -------------------------------------------------------------------------

KB_TEST("owner held: calls run inline under the owner's hold; the pass waits for the release") {
  OwnerRig r{};
  r.s.measure_value = 4U;
  KB_REQUIRE(r.oa.Health().Value().state == h::HealthState::Ready);
  const uint32_t beats = r.oa.Liveness().Beats();
  {
    h::BusHold<OwnerDomain> bus = r.domain.Hold(Milliseconds{0U});
    KB_REQUIRE(bus.OwnsLock());
    h::OwnerHeld<OA> held = r.oa.Held(bus);
    KB_REQUIRE(held.OwnsLock());
    const kb::Result<kbt::Reply> reply =
        held.Request(Command{.op = 3U, .arg = 6U}, Milliseconds{0U});
    KB_CHECK(reply.IsOk() && (reply.Value().value == 7U));
    const kb::Result<h::Snapshot<kbt::Reading>> sampled = held.Sample(Milliseconds{0U});
    KB_REQUIRE(sampled.IsOk());
    KB_CHECK_EQ(sampled.Value().value.value, 4U);
    KB_CHECK(held.Health().IsOk());
    KB_CHECK_EQ(r.domain.Service(kb::rtos::Now(), r.oa).Error(), Status::Busy);
    bus.Release();
    KB_CHECK(!held.OwnsLock());
    KB_CHECK_EQ(held.Request(Command{}, Milliseconds{0U}).Error(), Status::NotReady);
    KB_CHECK_EQ(held.Sample(Milliseconds{0U}).Error(), Status::NotReady);
  }
  KB_CHECK_EQ(r.s.runs, 1U);
  KB_CHECK_EQ(r.oa.Liveness().Beats(), beats + 2U);
  KB_CHECK(r.domain.Service(kb::rtos::Now(), r.oa).IsOk());
}

KB_TEST("owner held: a hold that is not had, or of another domain, refuses every call") {
  OwnerRig r{};
  h::BusHold<OwnerDomain> first = r.domain.Hold(Milliseconds{0U});
  KB_REQUIRE(first.OwnsLock());
  h::BusHold<OwnerDomain> second = r.domain.Hold(Milliseconds{0U});
  h::OwnerHeld<OA> refused = r.oa.Held(second);
  KB_CHECK(!refused.OwnsLock());
  KB_CHECK_EQ(refused.Request(Command{}, Milliseconds{0U}).Error(), Status::NotReady);

  Bus other_bus{};
  OwnerDomain other{other_bus};
  h::BusHold<OwnerDomain> elsewhere = other.Hold(Milliseconds{0U});
  KB_REQUIRE(elsewhere.OwnsLock());
  h::OwnerHeld<OA> wrong = r.oa.Held(elsewhere);
  KB_CHECK(!wrong.OwnsLock());
  KB_CHECK_EQ(wrong.Request(Command{}, Milliseconds{0U}).Error(), Status::NotReady);
  KB_CHECK_EQ(wrong.Sample(Milliseconds{0U}).Error(), Status::NotReady);
  KB_CHECK_EQ(r.s.runs, 0U);
  KB_CHECK_EQ(r.s.measures, 0U);
}

KB_TEST("owner held: a failing sample is forwarded; a held call before a start is NotReady") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  OA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  h::BusHold<OwnerDomain> hold = domain.Hold(Milliseconds{0U});
  h::OwnerHeld<OA> held = oa.Held(hold);
  KB_CHECK(held.OwnsLock());
  KB_CHECK_EQ(held.Request(Command{}, Milliseconds{0U}).Error(), Status::NotReady); // not started
  hold.Release();
  oa.RequestStart();
  KB_REQUIRE(domain.Service(kb::rtos::Now(), oa).IsOk());
  // cppcheck-suppress unreadVariable
  s.measure_status = Status::Nack;
  // cppcheck-suppress unreadVariable
  s.measure_detail = 5U;
  h::BusHold<OwnerDomain> again = domain.Hold(Milliseconds{0U});
  h::OwnerHeld<OA> sample = oa.Held(again);
  const kb::Result<h::Snapshot<kbt::Reading>> failed = sample.Sample(Milliseconds{0U});
  KB_CHECK_EQ(failed.Error(), Status::Nack);
  KB_CHECK_EQ(failed.Detail(), 5U);
}
