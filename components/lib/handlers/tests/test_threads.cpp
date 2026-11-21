// kb::handlers with real threads (kb::rtos::Thread on the host backend; also run under
// ThreadSanitizer): contention on one guarded handler and on two guarded handlers sharing one
// guarded bus, the lock order and the bounded timeouts, the owner model with several callers,
// request slots, stops and failing starts, a recovery storm on a guarded handler and recoveries
// through the owner task, seqlock readers against a writer (no torn value accepted), and held
// calls: a guarded hold against plain callers, an owner-bus hold against the owner's passes.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
// Hook signatures follow the core's (non-const driver, CRTP members the core reads); loops stay
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "handler_test_support.hpp"
#include "kb/rtos/thread.hpp"
#include "kb/rtos/time.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
using kbt::Command;
using kbt::Reply;
using kbt::Script;
using kbt::Toy;
namespace h = kb::handlers;
namespace host = kb::iface::host;

// Coverage builds: the same instrumented function run by two threads at the same instant loses
// counts with plain counters, and gcov then derives negative ones (a failed report), so this file
// is built with -fprofile-update=atomic there (CMakeLists.txt). The tests also keep the volume on
// shared lock-free paths low: the owner completes requests at least 150 us apart (each command
// takes that long), callers start out of step, the two handlers sharing a bus are of two types,
// and each lock-free state is read from one thread.

namespace {

using Bus = host::FakeI2cBus;
using OwnerDomain = h::BusDomain<Bus, h::ThreadModel::Owner>;
using GuardedDomain = h::BusDomain<Bus, h::ThreadModel::Guarded>;
using GToy = Toy<kbt::PlanSync>;
using G = h::Guarded<GToy, GuardedDomain>;

constexpr Milliseconds kLong{3000U};

uint64_t NowUs() noexcept {
  return kb::rtos::Now().Us();
}

// Waits at least `us` microseconds: the whole milliseconds asleep (the host tick is 1 ms), the rest
// yielding until the clock has passed it.
void SleepUs(uint32_t us) noexcept {
  const uint64_t end = NowUs() + us;
  if (us >= 1000U) {
    kb::rtos::SleepFor(Milliseconds{us / 1000U});
  }
  while (NowUs() < end) {
    kb::rtos::Yield();
  }
}

// Polls `done()` every 200 us for at most `ms` milliseconds.
template <typename Done>
bool WaitUntil(Done done, uint32_t ms) noexcept {
  const uint64_t end = NowUs() + (static_cast<uint64_t>(ms) * 1000U);
  bool ok = done();
  while ((!ok) && (NowUs() < end)) {
    SleepUs(200U);
    ok = done();
  }
  return ok;
}

h::HandlerConfig Config() noexcept {
  return h::HandlerConfig{};
}

template <typename F>
h::HealthRecord HealthOf(const F& facade) noexcept {
  const kb::Result<h::HealthRecord> r = facade.Health();
  return r.IsOk() ? r.Value() : h::HealthRecord{};
}

/// How long a test waits for one of its threads to end before it fails.
constexpr Milliseconds kJoin{60000U};

/// A thread of the tests: one kb::rtos::Thread step that runs `body()`, or `body(index)` for a body
/// taking the thread's index. The body must outlive the thread (declare it before the Task).
class Task : public kb::rtos::Thread<Task, 4096U> {
public:
  /// Starts the thread; false when the backend refused it.
  template <typename Body>
  [[nodiscard]] bool Launch(Body& body, uint32_t index = 0U) noexcept {
    body_ = &body;
    index_ = index;
    run_ = &invoke<Body>;
    return Start(kb::rtos::ThreadConfig{}).IsOk();
  }
  /// Waits for the body to return; false after kJoin.
  [[nodiscard]] bool Finish() noexcept {
    return Join(kJoin);
  }

  // The kb::rtos::Thread hooks.
  [[nodiscard]] bool Setup() const noexcept {
    return run_ != nullptr;
  }
  [[nodiscard]] bool Step() noexcept {
    run_(body_, index_);
    return false;
  }
  static void Cleanup() noexcept {}

private:
  template <typename Body>
  static void invoke(void* body, uint32_t index) noexcept {
    // cppcheck-suppress constVariableReference
    Body& b = *static_cast<Body*>(body);
    if constexpr (std::is_invocable_v<Body&, uint32_t>) {
      b(index);
    } else {
      b();
    }
  }

  void* body_{nullptr};
  uint32_t index_{0U};
  void (*run_)(void* body, uint32_t index) noexcept {nullptr};
};

/// Starts every task of `tasks` on `body`, each with its index; false when one did not start.
template <typename Body, std::size_t N>
[[nodiscard]] bool LaunchAll(std::array<Task, N>& tasks, Body& body) noexcept {
  bool ok = true;
  for (std::size_t i = 0U; i < N; ++i) {
    ok = tasks[i].Launch(body, static_cast<uint32_t>(i)) && ok;
  }
  return ok;
}

/// Waits for every task of `tasks`; false when one did not end in time.
template <std::size_t N>
[[nodiscard]] bool FinishAll(std::array<Task, N>& tasks) noexcept {
  bool ok = true;
  for (Task& t : tasks) {
    ok = t.Finish() && ok;
  }
  return ok;
}

/// A plain counter the handler's Run() adds to (protected only by the facade).
struct Counter {
  uint32_t value{0U};
};

kb::Result<Reply> AddToCounter(Script& script, const Command& command) noexcept {
  auto* counter = static_cast<Counter*>(script.user);
  counter->value += command.arg;
  return kb::Result<Reply>::Success(Reply{counter->value});
}

/// AddToCounter on an owner bus: the command takes 150 us, so the owner completes requests that far
/// apart.
kb::Result<Reply> SpacedAdd(Script& script, const Command& command) noexcept {
  SleepUs(150U);
  return AddToCounter(script, command);
}

/// The owner task of one owner-bus access: a service pass every 200 us, pausable.
template <typename Domain, typename Access>
class OwnerTask : public kb::rtos::Thread<OwnerTask<Domain, Access>, 4096U> {
  using Base = kb::rtos::Thread<OwnerTask<Domain, Access>, 4096U>;

public:
  OwnerTask(Domain& domain, Access& access) noexcept : domain_(domain), access_(access) {}
  OwnerTask(const OwnerTask&) = delete;
  OwnerTask& operator=(const OwnerTask&) = delete;
  OwnerTask(OwnerTask&&) = delete;
  OwnerTask& operator=(OwnerTask&&) = delete;
  ~OwnerTask() {
    Stop();
  }

  /// Starts the owner thread; false when the backend refused it.
  [[nodiscard]] bool Launch() noexcept {
    return Base::Start(kb::rtos::ThreadConfig{}).IsOk();
  }
  /// Ends the owner thread after its current pass and waits for it.
  void Stop() noexcept {
    Base::RequestStop();
    if (Base::State() != kb::rtos::ThreadState::Idle) {
      static_cast<void>(Base::Join(kJoin));
    }
  }

  // The kb::rtos::Thread hooks: one step is one pass (or one idle round while paused).
  static bool Setup() noexcept {
    return true;
  }
  [[nodiscard]] bool Step() noexcept {
    if (paused_.load(std::memory_order_acquire)) {
      idle_.store(true, std::memory_order_release);
    } else {
      idle_.store(false, std::memory_order_release);
      static_cast<void>(domain_.Service(kb::rtos::Now(), access_));
      static_cast<void>(passes_.fetch_add(1U, std::memory_order_relaxed));
    }
    SleepUs(200U);
    return true;
  }
  static void Cleanup() noexcept {}
  /// Pausing returns once no pass runs and none will start; resuming returns at once.
  [[nodiscard]] bool Pause(bool paused) noexcept {
    paused_.store(paused, std::memory_order_release);
    return (!paused) ||
           WaitUntil([this]() noexcept { return idle_.load(std::memory_order_acquire); }, 3000U);
  }
  [[nodiscard]] uint32_t Passes() const noexcept {
    return passes_.load(std::memory_order_relaxed);
  }

private:
  Domain& domain_;
  Access& access_;
  std::atomic<bool> paused_{false};
  std::atomic<bool> idle_{false};
  std::atomic<uint32_t> passes_{0U};
};

} // namespace

// --- Guarded -----------------------------------------------------------------------------------

KB_TEST("threads: several tasks on one guarded handler lose no update") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  Counter counter{};
  s.user = &counter;
  s.on_run = &AddToCounter;
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(kLong).IsOk());
  constexpr uint32_t kThreads = 4U;
  constexpr uint32_t kCalls = 250U;
  std::array<std::atomic<uint32_t>, kThreads> ok{};
  auto body = [&g, &ok](uint32_t t) noexcept {
    for (uint32_t i = 0U; i < kCalls; ++i) {
      if (g.Request(Command{.op = 0U, .arg = 1U}, kLong).IsOk()) {
        static_cast<void>(ok[t].fetch_add(1U, std::memory_order_relaxed));
      }
    }
  };
  std::array<Task, kThreads> threads{};
  KB_REQUIRE(LaunchAll(threads, body));
  KB_REQUIRE(FinishAll(threads));
  uint32_t total = 0U;
  for (const std::atomic<uint32_t>& n : ok) {
    // cppcheck-suppress useStlAlgorithm
    total += n.load();
  }
  KB_CHECK_EQ(total, kThreads * kCalls);
  KB_CHECK_EQ(counter.value, kThreads * kCalls);
  KB_CHECK_EQ(s.runs, kThreads * kCalls);
  KB_CHECK_EQ(g.LockTimeouts(), 0U);
  KB_CHECK_EQ(domain.Stats().holds, (kThreads * kCalls) + 1U);
  KB_CHECK_EQ(HealthOf(g).ok_calls, (kThreads * kCalls) + 1U);
  KB_CHECK_EQ(g.Liveness().Beats(), (kThreads * kCalls) + 1U);
}

namespace {

/// Detects two handlers on the bus at once.
struct BusWatch {
  std::atomic<bool> on_bus{false};
  std::atomic<uint32_t> overlaps{0U};
};

struct OverlapProbe {
  BusWatch* watch{nullptr};
  uint32_t calls{0U}; // the handler's own count, protected by its facade
};

// cppcheck-suppress constParameterCallback
kb::Result<Reply> OnBus(Script& script, const Command& command) noexcept {
  static_cast<void>(command);
  auto* probe = static_cast<OverlapProbe*>(script.user);
  if (probe->watch->on_bus.exchange(true, std::memory_order_acq_rel)) {
    static_cast<void>(probe->watch->overlaps.fetch_add(1U, std::memory_order_relaxed));
  }
  kb::rtos::Yield();
  ++probe->calls;
  probe->watch->on_bus.store(false, std::memory_order_release);
  return kb::Result<Reply>::Success(Reply{probe->calls});
}

constexpr uint32_t kHammerCalls = 150U;

template <typename F>
void Hammer(F& facade, std::atomic<uint32_t>& failures) noexcept {
  for (uint32_t i = 0U; i < kHammerCalls; ++i) {
    if (!facade.Request(Command{}, kLong).IsOk()) {
      static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
    }
  }
}

/// A second handler type, so the two handlers on one bus run separate copies of the facade code.
using G2 = h::Guarded<Toy<kbt::PlanSync, kbt::feat::kAll & ~kbt::feat::kFatal>, GuardedDomain>;

} // namespace

KB_TEST("threads: two guarded handlers on one guarded bus never use the bus at the same time") {
  Bus bus{};
  GuardedDomain domain{bus};
  BusWatch watch{};
  std::array<Script, 2> scripts{};
  std::array<OverlapProbe, 2> probes{};
  for (std::size_t i = 0U; i < 2U; ++i) {
    probes[i].watch = &watch;
    scripts[i].user = &probes[i];
    scripts[i].on_run = &OnBus;
  }
  G first{domain, scripts[0], Config()};
  G2 second{domain, scripts[1], Config()};
  KB_REQUIRE(domain.Create().IsOk() && first.Create().IsOk() && second.Create().IsOk());
  KB_REQUIRE(first.EnsureStarted(kLong).IsOk() && second.EnsureStarted(kLong).IsOk());
  constexpr uint32_t kPerHandler = 3U;
  constexpr uint32_t kCalls = kHammerCalls;
  std::atomic<uint32_t> failures{0U};
  auto on_first = [&first, &failures]() noexcept { Hammer(first, failures); };
  auto on_second = [&second, &failures]() noexcept { Hammer(second, failures); };
  std::array<Task, static_cast<std::size_t>(2U * kPerHandler)> threads{};
  for (std::size_t t = 0U; t < threads.size(); ++t) {
    if ((t % 2U) == 0U) {
      KB_REQUIRE(threads[t].Launch(on_first));
    } else {
      KB_REQUIRE(threads[t].Launch(on_second));
    }
  }
  KB_REQUIRE(FinishAll(threads));
  KB_CHECK_EQ(watch.overlaps.load(), 0U);
  KB_CHECK_EQ(failures.load(), 0U);
  KB_CHECK_EQ(probes[0].calls, kPerHandler * kCalls);
  KB_CHECK_EQ(probes[1].calls, kPerHandler * kCalls);
  KB_CHECK_EQ(domain.Stats().holds, (2U * kPerHandler * kCalls) + 2U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 0U);
}

KB_TEST("threads: the handler lock is taken before the bus lock, and both waits are bounded") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(kLong).IsOk());
  std::atomic<bool> held{false};
  std::atomic<bool> release{false};
  auto hold_bus = [&domain, &held, &release]() noexcept {
    const h::BusHold<GuardedDomain> hold = domain.Hold(kLong);
    held.store(hold.OwnsLock(), std::memory_order_release);
    static_cast<void>(WaitUntil([&release]() noexcept { return release.load(); }, 5000U));
  };
  Task holder{};
  KB_REQUIRE(holder.Launch(hold_bus));
  KB_REQUIRE(WaitUntil([&held]() noexcept { return held.load(); }, 2000U));

  // M holds the handler lock while it waits (bounded) for the bus.
  std::atomic<uint32_t> m_status{0xFFU};
  std::atomic<uint64_t> m_us{0U};
  std::atomic<bool> m_started{false};
  auto request = [&g, &m_status, &m_us, &m_started]() noexcept {
    const uint64_t t0 = NowUs();
    m_started.store(true, std::memory_order_release);
    m_status.store(static_cast<uint32_t>(g.Request(Command{}, Milliseconds{300U}).Error()));
    m_us.store(NowUs() - t0);
  };
  Task m{};
  KB_REQUIRE(m.Launch(request)); // a failed start: the holder ends by itself within 5 s
  KB_REQUIRE(WaitUntil(
      [&m_started]() noexcept { return m_started.load(std::memory_order_acquire); }, 2000U));
  SleepUs(40000U); // M takes the free handler lock at once, then waits for the bus
  // C cannot get the handler lock while M waits for the bus: it times out at the handler lock.
  const uint64_t c0 = NowUs();
  const kb::Result<Reply> c = g.Request(Command{}, Milliseconds{30U});
  const uint64_t c_us = NowUs() - c0;
  const bool m_ended = m.Finish();
  release.store(true);
  KB_REQUIRE(m_ended && holder.Finish());
  KB_CHECK_EQ(c.Error(), Status::Timeout);
  KB_CHECK(c_us >= 30000U);
  KB_CHECK(c_us < 2000000U);
  KB_CHECK_EQ(m_status.load(), static_cast<uint32_t>(Status::Timeout));
  KB_CHECK(m_us.load() >= 300000U);
  KB_CHECK(m_us.load() < 3000000U);
  KB_CHECK_EQ(g.LockTimeouts(), 2U);
  KB_CHECK_EQ(domain.Stats().lock_timeouts, 1U); // only M reached the bus lock
  KB_CHECK_EQ(s.runs, 0U);                       // nothing was sent
  KB_CHECK(g.Request(Command{}, kLong).IsOk());
}

// --- OwnerAccess -------------------------------------------------------------------------------

namespace {
using OToy = Toy<kbt::PlanSync>;
using OA4 = h::OwnerAccess<OToy, OwnerDomain, 4U>;
} // namespace

KB_TEST("threads: owner model with several caller threads: every request answered once") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  Counter counter{};
  s.user = &counter;
  s.on_run = &SpacedAdd;
  s.measure_value = 3U;
  OA4 oa{domain, s, Config()}; // the owner also samples on every pass
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, OA4> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  KB_REQUIRE(oa.EnsureStarted(kLong).IsOk());
  constexpr uint32_t kThreads = 4U;
  constexpr uint32_t kCalls = 50U;
  std::atomic<uint32_t> ok{0U};
  std::atomic<uint32_t> other{0U};
  std::atomic<uint32_t> backwards{0U};
  auto caller = [&oa, &ok, &other, &backwards](uint32_t t) noexcept {
    SleepUs(1000U * t); // out of step from the start
    uint32_t last_sequence = 0U;
    uint32_t last_reply = 0U;
    for (uint32_t i = 0U; i < kCalls; ++i) {
      const kb::Result<Reply> r = oa.Request(Command{.op = t, .arg = 1U}, kLong);
      // Each reply is the shared counter after this request: it only grows for each caller.
      const bool good = r.IsOk() && (r.Value().value > last_reply);
      last_reply = r.IsOk() ? r.Value().value : last_reply;
      static_cast<void>(good ? ok.fetch_add(1U) : other.fetch_add(1U));
      const kb::Result<h::Snapshot<kbt::Reading>> latest = oa.Latest();
      if (latest.IsOk()) {
        if (latest.Value().sequence < last_sequence) {
          static_cast<void>(backwards.fetch_add(1U));
        }
        last_sequence = latest.Value().sequence;
      }
    }
  };
  std::array<Task, kThreads> threads{};
  KB_REQUIRE(LaunchAll(threads, caller));
  KB_REQUIRE(FinishAll(threads));
  // The other requests, from this thread now that the callers are done.
  const kb::Result<h::Snapshot<kbt::Reading>> sampled = oa.Sample(kLong);
  KB_CHECK(sampled.IsOk() && (sampled.Value().value.value == 3U));
  KB_CHECK(oa.EnsureStarted(kLong).IsOk());
  owner.Stop();
  KB_CHECK_EQ(ok.load(), kThreads * kCalls);
  KB_CHECK_EQ(other.load(), 0U);
  KB_CHECK_EQ(backwards.load(), 0U);
  KB_CHECK_EQ(counter.value, kThreads * kCalls);
  KB_CHECK_EQ(s.runs, kThreads * kCalls);
  const h::ChannelStats st = oa.Requests();
  KB_CHECK_EQ(st.completed, (kThreads * kCalls) + 3U);
  KB_CHECK_EQ(st.timeouts, 0U);
  KB_CHECK_EQ(st.busy, 0U);
  KB_CHECK_EQ(domain.Stats().violations, 0U);
  KB_CHECK(oa.Liveness().Beats() >= 1U);
}

namespace {
using OA2 = h::OwnerAccess<OToy, OwnerDomain, 2U>;
} // namespace

KB_TEST("threads: owner Busy when every slot is in use; Timeout while the owner does not run") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  Counter counter{};
  s.user = &counter;
  s.on_run = &SpacedAdd;
  OA2 oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, OA2> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  KB_REQUIRE(oa.EnsureStarted(kLong).IsOk());
  KB_REQUIRE(owner.Pause(true));
  std::array<std::atomic<uint32_t>, 2> results{};
  results[0].store(0xFFU);
  results[1].store(0xFFU);
  auto caller = [&oa, &results](uint32_t i) noexcept {
    SleepUs(1000U * i); // out of step
    results[i].store(static_cast<uint32_t>(oa.Request(Command{}, Milliseconds{5000U}).Error()));
  };
  std::array<Task, 2> callers{};
  KB_REQUIRE(LaunchAll(callers, caller));
  KB_REQUIRE(WaitUntil([&oa]() noexcept { return oa.Requests().posted == 3U; }, 3000U));
  const uint64_t t0 = NowUs();
  KB_CHECK_EQ(oa.Request(Command{}, kLong).Error(), Status::Busy);
  KB_CHECK_EQ(oa.Stop(kLong).Error(), Status::Busy);
  KB_CHECK((NowUs() - t0) < 1000000U); // Busy does not wait for a slot
  KB_CHECK_EQ(oa.Requests().busy, 2U);
  static_cast<void>(owner.Pause(false));
  KB_REQUIRE(FinishAll(callers));
  KB_CHECK_EQ(results[0].load(), static_cast<uint32_t>(Status::Ok));
  KB_CHECK_EQ(results[1].load(), static_cast<uint32_t>(Status::Ok));

  KB_REQUIRE(owner.Pause(true));
  const uint64_t t1 = NowUs();
  KB_CHECK_EQ(oa.Request(Command{}, Milliseconds{20U}).Error(), Status::Timeout);
  KB_CHECK((NowUs() - t1) >= 20000U);
  KB_CHECK_EQ(oa.Requests().timeouts, 1U);
  static_cast<void>(owner.Pause(false));
  KB_CHECK(WaitUntil([&oa]() noexcept { return oa.Requests().skipped == 1U; }, 3000U));
  owner.Stop();
  KB_CHECK_EQ(s.runs, 2U); // the abandoned request was never run
}

KB_TEST("threads: every waiter of a failing start gets its failure") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].fail = Status::NoDevice;
  s.stages[0].fail_times = kbt::kAlways;
  s.stages[0].detail = 13U;
  using Polled = Toy<kbt::PlanPolled>;
  using PA = h::OwnerAccess<Polled, OwnerDomain, 4U>;
  s.stages[0].pending = 20U; // about 20 passes, then the failure
  s.stages[0].deadline_ms = 60000U;
  PA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, PA> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  std::array<std::atomic<uint32_t>, 3> results{};
  std::array<std::atomic<uint32_t>, 3> details{};
  auto caller = [&oa, &results, &details](uint32_t i) noexcept {
    SleepUs(1000U * i); // out of step
    const kb::Result<void> r = oa.EnsureStarted(Milliseconds{5000U});
    results[i].store(static_cast<uint32_t>(r.Error()));
    details[i].store(r.Detail());
  };
  std::array<Task, 3> callers{};
  KB_REQUIRE(LaunchAll(callers, caller));
  KB_REQUIRE(FinishAll(callers));
  owner.Stop();
  for (std::size_t i = 0U; i < callers.size(); ++i) {
    KB_CHECK_EQ(results[i].load(), static_cast<uint32_t>(Status::NoDevice));
    KB_CHECK_EQ(details[i].load(), 13U);
  }
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Faulted);
  KB_CHECK(HealthOf(oa).start_failures >= 1U);
}

KB_TEST("threads: Stop completes the pending start waiters with NotReady") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  s.stages[0].pending = kbt::kAlways;
  s.stages[0].deadline_ms = 60000U;
  using Polled = Toy<kbt::PlanPolled>;
  using PA = h::OwnerAccess<Polled, OwnerDomain, 4U>;
  PA oa{domain, s, Config()};
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, PA> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  std::array<std::atomic<uint32_t>, 2> results{};
  auto waiter = [&oa, &results](uint32_t i) noexcept {
    SleepUs(1000U * i); // out of step
    results[i].store(static_cast<uint32_t>(oa.EnsureStarted(Milliseconds{8000U}).Error()));
  };
  std::array<Task, 2> waiters{};
  KB_REQUIRE(LaunchAll(waiters, waiter));
  KB_REQUIRE(WaitUntil([&oa]() noexcept { return oa.Requests().posted == 2U; }, 3000U));
  // The start run began (the report is read by this thread only; the owner task reads the health
  // record).
  KB_REQUIRE(WaitUntil(
      [&oa]() noexcept { return oa.Startup().IsOk() && (oa.Startup().Value().runs == 1U); },
      3000U));
  SleepUs(5000U); // both start requests drained
  KB_CHECK(oa.Stop(kLong).IsOk());
  KB_REQUIRE(FinishAll(waiters));
  owner.Stop();
  KB_CHECK_EQ(results[0].load(), static_cast<uint32_t>(Status::NotReady));
  KB_CHECK_EQ(results[1].load(), static_cast<uint32_t>(Status::NotReady));
  KB_CHECK_EQ(HealthOf(oa).state, h::HealthState::Unknown);
  KB_CHECK_EQ(s.destroyed, s.emplaced);
}

// --- Recovery storms ---------------------------------------------------------------------------

namespace {

/// One start-up run as the chip saw it.
struct RunEntry {
  uint64_t at_us{0U};
  bool recovering{false};
  bool failed{false};
};

constexpr std::size_t kMaxRuns = 8192U;

/// The chip of a storm: start runs fail by pattern (or always), commands fail in pairs.
struct StormChip {
  h::HealthState (*state)(const void* facade) noexcept {nullptr};
  const void* facade{nullptr};
  std::atomic<bool> always_fail{false};
  uint32_t spacing_us{
      0U}; ///< How long each stage call and command takes (owner bus: spaced completions).
  uint32_t runs{0U};
  uint32_t commands{0U};
  std::array<RunEntry, kMaxRuns> log{};
  std::size_t logged{0U};
  bool overflow{false};
};

template <typename F>
h::HealthState StateOf(const void* facade) noexcept {
  return HealthOf(*static_cast<const F*>(facade)).state;
}

kb::Result<h::StartProgress> StormStage(Script& script, const h::StageCall& call) noexcept {
  auto* chip = static_cast<StormChip*>(script.user);
  if (chip->spacing_us != 0U) {
    SleepUs(chip->spacing_us);
  }
  const uint32_t run = chip->runs;
  ++chip->runs;
  const bool fail = chip->always_fail.load(std::memory_order_acquire) || ((run % 3U) != 0U);
  if (chip->logged < chip->log.size()) {
    RunEntry& e = chip->log[chip->logged];
    e.at_us = call.now.Us();
    e.recovering = chip->state(chip->facade) == h::HealthState::Recovering;
    e.failed = fail;
    ++chip->logged;
  } else {
    chip->overflow = true;
  }
  return fail ? kb::Result<h::StartProgress>::Failure(Status::NoDevice, 1U)
              : kb::Result<h::StartProgress>::Success(h::StartProgress::Done);
}

kb::Result<Reply> StormRun(Script& script, const Command& command) noexcept {
  static_cast<void>(command);
  auto* chip = static_cast<StormChip*>(script.user);
  if (chip->spacing_us != 0U) {
    SleepUs(chip->spacing_us);
  }
  const uint32_t n = chip->commands;
  ++chip->commands;
  return ((n % 4U) >= 2U) ? kb::Result<Reply>::Failure(Status::BusError)
                          : kb::Result<Reply>::Success(Reply{n});
}

/// The replies of a storm's RequestRecovery calls, by status.
struct StormTally {
  std::atomic<uint32_t> ok{0U};
  std::atomic<uint32_t> failed{0U}; // the recovery run failed (NoDevice)
  std::atomic<uint32_t> busy{0U};
  std::atomic<uint32_t> fault{0U};
  std::atomic<uint32_t> fault_early{0U}; // Fault before the chip failed for good
  std::atomic<uint32_t> other{0U};
  void Count(Status status, bool exhausted_possible) noexcept {
    switch (status) {
    case Status::Ok:
      static_cast<void>(ok.fetch_add(1U));
      break;
    case Status::NoDevice:
      static_cast<void>(failed.fetch_add(1U));
      break;
    case Status::Busy:
      static_cast<void>(busy.fetch_add(1U));
      break;
    case Status::Fault:
      static_cast<void>(fault.fetch_add(1U));
      if (!exhausted_possible) {
        static_cast<void>(fault_early.fetch_add(1U));
      }
      break;
    case Status::InvalidArgument:
    case Status::OutOfRange:
    case Status::Timeout:
    case Status::NotReady:
    case Status::Nack:
    case Status::BusError:
    case Status::Overflow:
    case Status::Full:
    case Status::Empty:
    case Status::Unsupported:
    case Status::AlreadyStarted:
    case Status::CrcMismatch:
    default:
      static_cast<void>(other.fetch_add(1U));
      break;
    }
  }
};

/// Checks a run log against the recovery policy: each recovery run after a failed one waits the
/// doubled backoff (capped), and no more than `max_attempts` recovery runs fail in a row.
struct LogVerdict {
  uint32_t recovery_runs{0U};
  uint32_t recovery_ok{0U};
  uint32_t recovery_failed{0U};
  uint32_t backoff_violations{0U};
  uint32_t max_streak{0U};
  uint32_t final_streak{0U};
};

LogVerdict Verify(const StormChip& chip, const h::RecoveryPolicy& policy) noexcept {
  LogVerdict v{};
  uint32_t streak = 0U;
  uint64_t last_failure_us = 0U;
  for (std::size_t i = 0U; i < chip.logged; ++i) {
    const RunEntry& e = chip.log[i];
    if (e.recovering) {
      ++v.recovery_runs;
      if (streak != 0U) {
        uint64_t wait_ms = policy.backoff.Count();
        for (uint32_t k = 1U; k < streak; ++k) {
          wait_ms = (wait_ms >= policy.max_backoff.Count()) ? wait_ms : (wait_ms * 2U);
        }
        wait_ms = (wait_ms > policy.max_backoff.Count()) ? policy.max_backoff.Count() : wait_ms;
        if ((e.at_us - last_failure_us) < (wait_ms * 1000U)) {
          ++v.backoff_violations;
        }
      }
      if (e.failed) {
        ++v.recovery_failed;
        ++streak;
        last_failure_us = e.at_us;
      } else {
        ++v.recovery_ok;
        streak = 0U;
      }
      v.max_streak = (streak > v.max_streak) ? streak : v.max_streak;
    } else {
      streak = e.failed ? streak : 0U;
    }
  }
  v.final_streak = streak;
  return v;
}

} // namespace

KB_TEST("threads: a recovery storm on a guarded handler honours backoff and attempts, and ends "
        "consistent") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  static StormChip chip{};
  chip.state = &StateOf<G>;
  s.user = &chip;
  s.on_stage = &StormStage;
  s.on_run = &StormRun;
  h::HandlerConfig config = Config();
  config.health.fault_after = 2U;
  config.recovery.backoff = Milliseconds{2U};
  config.recovery.max_backoff = Milliseconds{6U};
  config.recovery.max_attempts = 3U;
  G g{domain, s, config};
  chip.facade = &g;
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(kLong).IsOk()); // run 0 succeeds
  StormTally tally{};
  std::atomic<bool> stop{false};
  std::atomic<bool> exhaustible{false};
  std::atomic<uint32_t> lock_timeouts{0U};
  constexpr std::size_t kThreads = 6U;
  auto storm = [&g, &tally, &stop, &exhaustible, &lock_timeouts]() noexcept {
    while (!stop.load(std::memory_order_acquire)) {
      const kb::Result<Reply> r = g.Request(Command{}, kLong);
      if (r.Error() == Status::NotReady) {
        const Status rec = g.RequestRecovery(kLong).Error();
        tally.Count(rec, exhaustible.load(std::memory_order_acquire));
      } else if (r.Error() == Status::Timeout) {
        static_cast<void>(lock_timeouts.fetch_add(1U));
      } else {
        // Served.
      }
      SleepUs(30U);
    }
  };
  std::array<Task, kThreads> threads{};
  const bool launched = LaunchAll(threads, storm);
  if (!launched) {
    stop.store(true,
               std::memory_order_release); // the started ones end before the return joins them
  }
  KB_REQUIRE(launched);
  SleepUs(300000U); // intermittent: at most two failed runs in a row
  exhaustible.store(true, std::memory_order_release);
  chip.always_fail.store(true, std::memory_order_release);
  SleepUs(200000U); // failing for good: the attempts run out
  stop.store(true, std::memory_order_release);
  KB_REQUIRE(FinishAll(threads));
  KB_CHECK(!chip.overflow);
  const LogVerdict v = Verify(chip, config.recovery);
  const h::HealthRecord health = HealthOf(g);
  KB_CHECK_EQ(v.backoff_violations, 0U);
  KB_CHECK_EQ(v.max_streak, config.recovery.max_attempts);
  KB_CHECK_EQ(v.final_streak, config.recovery.max_attempts);
  KB_CHECK(v.recovery_ok >= 1U);
  KB_CHECK_EQ(tally.fault_early.load(), 0U);
  KB_CHECK(tally.fault.load() >= 1U);
  KB_CHECK_EQ(tally.ok.load(), v.recovery_ok);
  KB_CHECK_EQ(tally.failed.load(), v.recovery_failed);
  KB_CHECK_EQ(tally.other.load(), 0U);
  KB_CHECK_EQ(lock_timeouts.load(), 0U);
  KB_CHECK_EQ(health.state, h::HealthState::Faulted);
  KB_CHECK_EQ(health.recoveries, v.recovery_ok);
  KB_CHECK_EQ(health.recovery_failures, v.recovery_failed);
  KB_CHECK_EQ(health.starts + health.start_failures, static_cast<uint32_t>(chip.logged));
  KB_CHECK_EQ(g.LockTimeouts(), 0U);
}

namespace {
using OA8 = h::OwnerAccess<OToy, OwnerDomain, 8U>;
} // namespace

KB_TEST("threads: recoveries asked by one task while another task's requests run, through the "
        "owner task") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  static StormChip chip{};
  chip.state = &StateOf<OA8>;
  chip.spacing_us = 150U;
  s.user = &chip;
  s.on_stage = &StormStage;
  s.on_run = &StormRun;
  h::HandlerConfig config = Config();
  config.health.fault_after = 2U;
  config.recovery.backoff = Milliseconds{1U};
  config.recovery.max_backoff =
      Milliseconds{1U};              // shorter than the recovering task's pace: never Busy
  config.recovery.max_attempts = 0U; // no limit
  OA8 oa{domain, s, config};
  chip.facade = &oa;
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, OA8> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  KB_REQUIRE(oa.EnsureStarted(kLong).IsOk());
  StormTally tally{};
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> requests{0U};
  std::atomic<uint32_t> served{0U};
  std::atomic<uint32_t> refused{0U};
  std::atomic<uint32_t> other{0U};
  auto request = [&oa, &stop, &requests, &served, &refused, &other]() noexcept {
    while (!stop.load(std::memory_order_acquire)) {
      const Status r = oa.Request(Command{}, kLong).Error();
      static_cast<void>(requests.fetch_add(1U));
      if ((r == Status::Ok) || (r == Status::BusError)) {
        static_cast<void>(served.fetch_add(1U));
      } else if (r == Status::NotReady) {
        static_cast<void>(refused.fetch_add(1U));
        SleepUs(500U);
      } else {
        static_cast<void>(other.fetch_add(1U));
      }
    }
  };
  auto recover = [&oa, &tally, &stop, &requests]() noexcept {
    SleepUs(700U); // out of step
    while (!stop.load(std::memory_order_acquire)) {
      tally.Count(oa.RequestRecovery(kLong).Error(), true);
      static_cast<void>(requests.fetch_add(1U));
      SleepUs(1500U);
    }
  };
  Task requester{};
  Task recoverer{};
  const bool launched = requester.Launch(request) && recoverer.Launch(recover);
  if (launched) {
    SleepUs(300000U);
  }
  stop.store(true, std::memory_order_release);
  KB_REQUIRE(launched);
  KB_REQUIRE(requester.Finish() && recoverer.Finish());
  owner.Stop();
  KB_CHECK(!chip.overflow);
  const LogVerdict v = Verify(chip, config.recovery);
  const h::HealthRecord health = HealthOf(oa);
  KB_CHECK_EQ(v.backoff_violations, 0U);
  KB_CHECK(v.recovery_ok >= 1U);
  KB_CHECK(v.recovery_failed >= 1U);
  KB_CHECK(served.load() >= 1U);
  KB_CHECK(refused.load() >= 1U);
  KB_CHECK_EQ(other.load(), 0U);
  KB_CHECK_EQ(health.recoveries, v.recovery_ok);
  KB_CHECK_EQ(health.recovery_failures, v.recovery_failed);
  KB_CHECK_EQ(tally.ok.load(), v.recovery_ok);
  KB_CHECK_EQ(tally.failed.load(), v.recovery_failed);
  KB_CHECK_EQ(tally.busy.load() + tally.fault.load() + tally.other.load(), 0U);
  const h::ChannelStats st = oa.Requests();
  KB_CHECK_EQ(st.completed, requests.load() + 1U); // + the first start
  KB_CHECK_EQ(st.timeouts, 0U);
  KB_CHECK_EQ(st.busy, 0U);
  KB_CHECK_EQ(domain.Stats().violations, 0U);
}

// --- Seqlock readers and a writer --------------------------------------------------------------

namespace {

struct Wide {
  std::array<uint32_t, 256> words{};
};

bool Uniform(const Wide& w) noexcept {
  bool same = true;
  for (const uint32_t word : w.words) {
    same = same && (word == w.words[0]);
  }
  return same;
}

} // namespace

KB_TEST("threads: seqlock readers never accept a torn value; torn reads are retried, then Busy") {
  static h::Seqlock<Wide> cell{};
  std::atomic<bool> stop{false};
  std::atomic<bool> written{false};
  // A bounded number of publishes keeps the counts of a coverage build far from gcov's suspicious
  // range.
  auto write = [&stop, &written]() noexcept {
    static Wide value{};
    uint32_t generation = 0U;
    while ((!stop.load(std::memory_order_acquire)) && (generation < 200000U)) {
      ++generation;
      value.words.fill(generation);
      cell.Publish(value);
    }
    written.store(true, std::memory_order_release);
  };
  Task writer{};
  KB_REQUIRE(writer.Launch(write)); // a failed start: nothing else runs
  std::atomic<uint32_t> torn{0U};
  std::atomic<uint32_t> busy{0U};
  std::atomic<uint32_t> clean{0U};
  std::atomic<uint32_t> bad{0U};
  auto reading = [&torn, &busy, &clean, &bad, &written]() noexcept {
    static thread_local Wide out{};
    uint32_t last = 0U;
    const uint64_t end = NowUs() + 3000000U;
    bool enough = false;
    while ((!enough) && (NowUs() < end)) {
      uint32_t publishes = 0U;
      if (cell.TryRead(out, publishes)) {
        static_cast<void>(clean.fetch_add(1U));
        if ((!Uniform(out)) || (out.words[0] < last)) {
          static_cast<void>(bad.fetch_add(1U));
        }
        last = out.words[0];
      } else {
        static_cast<void>(torn.fetch_add(1U));
      }
      const kb::Result<Wide> read = cell.Read();
      if (!read.IsOk()) {
        static_cast<void>(busy.fetch_add(1U));
      } else if (!Uniform(read.Value())) {
        static_cast<void>(bad.fetch_add(1U));
      } else {
        // A whole value.
      }
      enough = ((torn.load() > 100U) && (busy.load() > 10U) && (clean.load() > 10U)) ||
               written.load(std::memory_order_acquire);
    }
  };
  std::array<Task, 1> readers{}; // one reader: see the note on coverage counters
  const bool launched = LaunchAll(readers, reading);
  const bool read_all = FinishAll(readers);
  stop.store(true, std::memory_order_release);
  KB_REQUIRE(launched && read_all && writer.Finish());
  KB_CHECK_EQ(bad.load(), 0U);
  KB_CHECK(torn.load() > 0U);
  KB_CHECK(busy.load() > 0U);
  // Whether a reader sees a clean copy while the writer never pauses depends on the scheduler; once
  // the writer stopped, every read is clean.
  Wide last{};
  uint32_t publishes = 0U;
  KB_CHECK(cell.TryRead(last, publishes));
  KB_CHECK(Uniform(last));
  KB_CHECK(cell.Read().IsOk());
}

namespace {

/// A handler with a wide snapshot, sampled from its one writing thread.
class WideToy : public h::HandlerCore<WideToy, kbt::ToyDriver, Wide> {
public:
  // cppcheck-suppress unusedStructMember
  static constexpr auto kStartupPlan = kbt::PlanSync::kPlan;
  static constexpr bool kRequiresOwner = false;
  static constexpr h::BusKind kBus = h::BusKind::I2c;
  explicit WideToy(Script& script) noexcept
      : h::HandlerCore<WideToy, kbt::ToyDriver, Wide>(Config()), script_(script) {}
  void EmplaceDriver(std::optional<kbt::ToyDriver>& slot) noexcept {
    slot.emplace(script_);
  }
  [[nodiscard]] static kb::Result<h::StartProgress> RunStage(
      const h::StageCall& call,
      // cppcheck-suppress constParameterReference
      kbt::ToyDriver& driver) noexcept {
    static_cast<void>(call);
    static_cast<void>(driver);
    return kb::Result<h::StartProgress>::Success(h::StartProgress::Done);
  }
  [[nodiscard]] static kb::Milliseconds StageDeadline(std::size_t index) noexcept {
    static_cast<void>(index);
    return kb::Milliseconds{10U};
  }
  // cppcheck-suppress constParameterReference
  [[nodiscard]] kb::Result<Wide> Measure(kbt::ToyDriver& driver, kb::Instant now) noexcept {
    static_cast<void>(driver);
    static_cast<void>(now);
    ++generation_;
    value_.words.fill(generation_);
    return kb::Result<Wide>::Success(value_);
  }

private:
  Script& script_;
  Wide value_{};
  uint32_t generation_{0U};
};

static_assert(h::Handler<WideToy> && (!WideToy::kRequiresOwner) &&
              (WideToy::kBus == h::BusKind::I2c));

} // namespace

KB_TEST(
    "threads: Latest under a concurrent writer gives a whole snapshot or Busy, never a torn one") {
  static Script s{};
  static WideToy handler{s};
  static const h::ServiceKey kKey = h::ServiceKeyTestPeer::Make();
  KB_REQUIRE(handler.EnsureInitialized(kKey).IsOk());
  KB_REQUIRE(handler.Sample(kKey, kb::rtos::Now()).IsOk());
  std::atomic<bool> stop{false};
  std::atomic<bool> written{false};
  auto write = [&stop, &written]() noexcept {
    uint32_t samples = 0U;
    while ((!stop.load(std::memory_order_acquire)) && (samples < 400000U)) {
      static_cast<void>(handler.Sample(kKey, kb::rtos::Now()));
      ++samples;
    }
    written.store(true, std::memory_order_release);
  };
  Task writer{};
  KB_REQUIRE(writer.Launch(write));
  uint32_t busy = 0U;
  uint32_t whole = 0U;
  uint32_t bad = 0U;
  uint32_t last = 0U;
  const uint64_t end = NowUs() + 3000000U;
  // Reads go on until the writer has made all its samples (or the time is up), whatever the counts: a torn read
  // is rare, and a seqlock that accepted one must be given every chance to show it.
  while ((NowUs() < end) && (!written.load(std::memory_order_acquire))) {
    const kb::Result<h::Snapshot<Wide>> latest = handler.Latest();
    if (latest.IsOk()) {
      ++whole;
      const Wide& w = latest.Value().value;
      bad += (Uniform(w) && (w.words[0] >= last)) ? 0U : 1U;
      last = w.words[0];
    } else if (latest.Error() == Status::Busy) {
      ++busy;
    } else {
      ++bad;
    }
  }
  stop.store(true, std::memory_order_release);
  KB_REQUIRE(writer.Finish());
  // With the writer stopped no read can be torn: this one is whole, whatever the schedule was.
  const kb::Result<h::Snapshot<Wide>> settled = handler.Latest();
  KB_REQUIRE(settled.IsOk());
  const Wide& final_value = settled.Value().value;
  bad += (Uniform(final_value) && (final_value.words[0] >= last)) ? 0U : 1U;
  ++whole;
  KB_CHECK_EQ(bad, 0U);
  KB_CHECK(whole > 0U);
  KB_CHECK(busy > 0U);
}

// --- Request channel: a completion racing the caller's timeout ---------------------------------

KB_TEST("threads: a completion racing the caller's timeout loses no slot and crosses no reply") {
  static h::RequestChannel<Command, Reply, 2U> ch{};
  KB_REQUIRE(ch.Create().IsOk());
  std::atomic<bool> stop{false};
  auto serve = [&stop]() noexcept {
    uint32_t jitter = 0U;
    while (!stop.load(std::memory_order_acquire)) {
      h::TakenRequest<Command> taken{};
      if (ch.Next(taken)) {
        jitter = (jitter + 337U) % 3600U;
        SleepUs(200U + jitter); // before and after the caller's 2-tick wait
        ch.Complete(taken.slot, kb::Result<Reply>::Success(Reply{taken.command.op}));
      } else {
        SleepUs(50U);
      }
    }
  };
  Task owner{};
  KB_REQUIRE(owner.Launch(serve));
  uint32_t ok = 0U;
  uint32_t timeouts = 0U;
  uint32_t crossed = 0U;
  uint32_t stale = 0U;
  for (uint32_t i = 1U; i <= 150U; ++i) {
    const kb::Result<uint32_t> slot = ch.Post(h::RequestKind::Command, Command{.op = i, .arg = 0U});
    if (!slot.IsOk()) {
      SleepUs(3000U); // both slots wait for the owner: give it time
      continue;
    }
    // Most waits are 1 ms, shorter than the owner's 0.2 to 3.8 ms service time, so completions race
    // the timeout. Every fifth wait is long enough to see its reply even on a loaded machine: the
    // test then always sees both outcomes (ok and timeout) instead of depending on the scheduler (a
    // 1 ms-only loop timed out 150 of 150 times under load now and then).
    const Milliseconds wait = ((i % 5U) == 0U) ? Milliseconds{200U} : Milliseconds{1U};
    const kb::Result<Reply> r = ch.Wait(slot.Value(), wait);
    if (r.IsOk()) {
      ++ok;
      crossed += (r.Value().value == i) ? 0U : 1U;
    } else if (r.Error() == Status::Timeout) {
      ++timeouts;
    } else {
      ++stale; // a late flag taken for this request's completion: must never happen
    }
  }
  const bool drained = WaitUntil(
      []() noexcept {
        return (ch.StateOf(0U) == h::SlotState::Free) && (ch.StateOf(1U) == h::SlotState::Free);
      },
      2000U);
  stop.store(true, std::memory_order_release);
  KB_REQUIRE(owner.Finish());
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(crossed, 0U);
  KB_CHECK(ok > 0U);
  KB_CHECK(timeouts > 0U);
  KB_CHECK_EQ(st.completed, ok);
  KB_CHECK_EQ(st.timeouts, timeouts);
  // A caller that saw Done just after its wait consumes the late flag, so a later request on the
  // same slot never takes it for its own completion.
  KB_CHECK_EQ(stale, 0U);
  KB_CHECK(drained);
  KB_CHECK_EQ(st.posted, ok + timeouts);
}

// --- Held calls (ADR-0015) ---------------------------------------------------------------------

KB_TEST("threads: a guarded hold keeps every other task's call out of its sequence") {
  Bus bus{};
  GuardedDomain domain{bus};
  Script s{};
  Counter counter{};
  s.user = &counter;
  s.on_run = &AddToCounter;
  G g{domain, s, Config()};
  KB_REQUIRE(domain.Create().IsOk() && g.Create().IsOk());
  KB_REQUIRE(g.EnsureStarted(kLong).IsOk());
  constexpr uint32_t kRounds = 40U;
  constexpr uint32_t kSteps = 4U;
  constexpr uint32_t kCalls = 120U;
  std::atomic<uint32_t> torn{0U};
  std::atomic<uint32_t> held_rounds{0U};
  std::atomic<uint32_t> plain_ok{0U};
  // Each held round adds kSteps; the replies are the counter after each step, so any call of
  // another task in between shows as a gap.
  auto holder = [&g, &torn, &held_rounds](uint32_t t) noexcept {
    SleepUs(300U * t);
    for (uint32_t i = 0U; i < kRounds; ++i) {
      h::GuardedHeld<G> held = g.Hold(kLong);
      if (held.OwnsLock()) {
        uint32_t first = 0U;
        uint32_t last = 0U;
        for (uint32_t k = 0U; k < kSteps; ++k) {
          const kb::Result<Reply> r = held.Request(Command{.op = t, .arg = 1U}, kLong);
          last = r.ValueOr(Reply{}).value;
          first = (k == 0U) ? last : first;
        }
        static_cast<void>((last == (first + kSteps - 1U)) ? held_rounds.fetch_add(1U)
                                                          : torn.fetch_add(1U));
      }
      SleepUs(100U);
    }
  };
  auto plain = [&g, &plain_ok](uint32_t t) noexcept {
    SleepUs(200U * t);
    for (uint32_t i = 0U; i < kCalls; ++i) {
      if (g.Request(Command{.op = 9U, .arg = 1U}, kLong).IsOk()) {
        static_cast<void>(plain_ok.fetch_add(1U));
      }
    }
  };
  std::array<Task, 2> holders{};
  std::array<Task, 2> plains{};
  KB_REQUIRE(LaunchAll(holders, holder));
  KB_REQUIRE(LaunchAll(plains, plain));
  KB_REQUIRE(FinishAll(holders));
  KB_REQUIRE(FinishAll(plains));
  KB_CHECK_EQ(torn.load(), 0U);
  KB_CHECK_EQ(held_rounds.load(), 2U * kRounds);
  KB_CHECK_EQ(plain_ok.load(), 2U * kCalls);
  KB_CHECK_EQ(counter.value, (2U * kRounds * kSteps) + (2U * kCalls));
  KB_CHECK_EQ(g.LockTimeouts(), 0U);
}

KB_TEST("threads: an owner-bus hold taken from another task and the owner's passes never overlap") {
  Bus bus{};
  OwnerDomain domain{bus};
  Script s{};
  Counter counter{};
  s.user = &counter;
  s.on_run = &AddToCounter;
  s.measure_value = 5U;
  OA4 oa{domain, s, Config()}; // the owner samples on every pass: it writes the handler too
  KB_REQUIRE(oa.Create().IsOk());
  OwnerTask<OwnerDomain, OA4> owner{domain, oa};
  KB_REQUIRE(owner.Launch());
  KB_REQUIRE(oa.EnsureStarted(kLong).IsOk());
  uint32_t held_calls = 0U;
  uint32_t refused = 0U;
  for (uint32_t i = 0U; i < 300U; ++i) {
    h::BusHold<OwnerDomain> hold = domain.Hold(Milliseconds{0U});
    h::OwnerHeld<OA4> held = oa.Held(hold);
    if (held.OwnsLock()) {
      KB_CHECK(held.Request(Command{.op = 1U, .arg = 1U}, kLong).IsOk());
      KB_CHECK(held.Sample(kLong).IsOk());
      ++held_calls;
    } else {
      ++refused; // a pass was running: the hold never waits
    }
    hold.Release();
    SleepUs(50U);
  }
  owner.Stop();
  KB_CHECK(held_calls > 0U);
  KB_CHECK(owner.Passes() > 0U);
  KB_CHECK_EQ(counter.value, held_calls);
  KB_CHECK_EQ(s.runs, held_calls);
  const h::OwnerStats st = domain.Stats();
  KB_CHECK_EQ(st.holds, held_calls);
  KB_CHECK(st.violations >= refused); // refused holds, and passes refused during a hold
}
