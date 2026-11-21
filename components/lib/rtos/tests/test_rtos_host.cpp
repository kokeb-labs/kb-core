// The threaded suite of kb::rtos: real threads, real contention, at-least timing. Built on the
// std::thread host backend (also run under ThreadSanitizer in the sanitizer job) and on the real
// ThreadX kernel's Linux port (KB_THREADX_HOST_TESTS). SPDX-License-Identifier: MIT Copyright (c)
// 2026 The kb-core authors
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "counting_thread.hpp"
#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;

namespace {

// The tick the backend runs at (the ThreadX Linux build passes its kernel's), and how far a bounded
// wait may overrun its timeout: two ticks on a tick clock (ThreadX), plus scheduling slack on the
// wall clock of the std::thread backend.
#if defined(KB_TEST_TICK_HZ)
constexpr uint32_t kTickHz = KB_TEST_TICK_HZ;
constexpr uint64_t kSlackUs = 0U;
#else
constexpr uint32_t kTickHz = 1000U;
constexpr uint64_t kSlackUs = 30000U;
#endif
constexpr uint64_t kTickUs = 1000000U / kTickHz;

// A thread whose Setup takes 300 ms.
class SlowSetup : public r::Thread<SlowSetup, 4096U> {
public:
  static bool Setup() noexcept {
    r::SleepFor(Milliseconds{300U});
    return true;
  }
  static bool Step() noexcept {
    return false;
  }
  static void Cleanup() noexcept {}
};

class Producer : public r::Thread<Producer, 4096U> {
public:
  bool Setup() const noexcept {
    return queue != nullptr;
  }
  bool Step() noexcept {
    const bool sent = queue->SendFor(next, Milliseconds{500U});
    if (sent) {
      ++next;
    }
    return next < 100U;
  }
  void Cleanup() const noexcept {
    flags->Set(0x1U);
  }
  r::Queue<uint32_t, 4>* queue{nullptr};
  r::EventFlags* flags{nullptr};
  uint32_t next{0U};
};

class Holder : public r::Thread<Holder, 4096U> {
public:
  bool Setup() const noexcept {
    return mutex->TryLockFor(Milliseconds{100U});
  }
  bool Step() noexcept {
    held.store(true);
    return !StopRequested();
  }
  void Cleanup() const noexcept {
    mutex->Unlock();
  }
  r::Mutex* mutex{nullptr};
  std::atomic<bool> held{false};
};

class RecursiveHolder : public r::Thread<RecursiveHolder, 4096U> {
public:
  bool Setup() const noexcept {
    return mutex->TryLockFor(Milliseconds{100U}) && mutex->TryLockFor(Milliseconds{0U});
  }
  bool Step() noexcept {
    held.store(true);
    return !StopRequested();
  }
  void Cleanup() const noexcept {
    mutex->Unlock();
    mutex->Unlock();
  }
  r::RecursiveMutex* mutex{nullptr};
  std::atomic<bool> held{false};
};

// One side of a contention round: waits on its bit with short timeouts and sets the other side's,
// on a group or a semaphore that later rounds build again in the same bytes.
class Contender : public r::Thread<Contender, 4096U> {
public:
  [[nodiscard]] bool Setup() const noexcept {
    return (flags != nullptr) || (semaphore != nullptr);
  }
  bool Step() noexcept {
    if (flags != nullptr) {
      flags->Clear(mine);
      static_cast<void>(flags->WaitAny(mine, Milliseconds{(steps % 2U) == 0U ? 0U : 1U}));
      flags->Set(other);
    } else {
      static_cast<void>(semaphore->TakeFor(Milliseconds{(steps % 2U) == 0U ? 0U : 1U}));
      static_cast<void>(semaphore->Give());
    }
    ++steps;
    return steps < kSteps;
  }
  static void Cleanup() noexcept {}
  static constexpr uint32_t kSteps = 300U;
  r::EventFlags* flags{nullptr};
  r::Semaphore* semaphore{nullptr};
  uint32_t mine{0U};
  uint32_t other{0U};
  uint32_t steps{0U};
};

// Bytes that every round builds its group or its semaphore in.
using SharedBytes = std::array<std::byte, std::max(sizeof(r::EventFlags), sizeof(r::Semaphore))>;
SharedBytes& Shared() noexcept {
  alignas(std::max(alignof(r::EventFlags), alignof(r::Semaphore))) static SharedBytes bytes{};
  return bytes;
}

// One round: two contenders on a new object in Shared(), joined, then the object's end.
template <typename T>
bool ContendInShared() noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): placement into the shared bytes
  T* const object = ::new (static_cast<void*>(Shared().data())) T{};
  bool ok = false;
  if constexpr (std::is_same_v<T, r::EventFlags>) {
    ok = object->Create().IsOk();
  } else {
    ok = object->Create(1U, 1U).IsOk();
  }
  {
    std::array<Contender, 2> sides{};
    for (uint32_t i = 0U; i < 2U; ++i) {
      if constexpr (std::is_same_v<T, r::EventFlags>) {
        sides[i].flags = object;
      } else {
        sides[i].semaphore = object;
      }
      sides[i].mine = 1U << i;
      sides[i].other = 1U << (1U - i);
    }
    ok = ok && sides[0].Start(r::ThreadConfig{}).IsOk() && sides[1].Start(r::ThreadConfig{}).IsOk();
    ok = sides[0].Join(Milliseconds{20000U}) && sides[1].Join(Milliseconds{20000U}) && ok;
  }
  object->~T();
  return ok;
}

} // namespace

KB_TEST("host: groups and semaphores built again and again in the same bytes under contention "
        "keep working (each end deletes the native object, ADR-0017)") {
  bool ok = true;
  for (uint32_t round = 0U; round < 60U; ++round) {
    ok = (((round % 2U) == 0U) ? ContendInShared<r::EventFlags>()
                               : ContendInShared<r::Semaphore>()) &&
         ok;
  }
  KB_CHECK(ok);
}

KB_TEST("host: a timer's callback never runs after the timer has ended") {
  static std::atomic<uint32_t> fired{0U};
  {
    r::Timer t{};
    KB_REQUIRE(
        t.Create([](void* c) noexcept { static_cast<std::atomic<uint32_t>*>(c)->fetch_add(1U); },
                 &fired, Milliseconds{1U}, true)
            .IsOk());
    KB_CHECK(t.Start());
    for (uint32_t waited = 0U; (fired.load() < 2U) && (waited < 2000U); ++waited) {
      r::SleepFor(Milliseconds{1U});
    }
    KB_CHECK(fired.load() >= 2U);
  } // the destructor deletes the timer while it runs
  const uint32_t at_end = fired.load();
  r::SleepFor(Milliseconds{30U});
  KB_CHECK_EQ(fired.load(), at_end);
}

KB_TEST("host: sleeps wait at least the time asked") {
  const kb::Instant t0 = r::Now();
  r::SleepFor(Milliseconds{20U});
  KB_CHECK(kb::utils::ElapsedUs(r::Now(), t0) >= 20000U);
  r::SleepFor(Milliseconds{0U});
  r::Yield();
  KB_CHECK_EQ(r::TickRateHz(), kTickHz);
}

KB_TEST("host: a queue carries every item from a producer thread in order") {
  static r::Queue<uint32_t, 4> q{};
  static r::EventFlags done{};
  KB_REQUIRE(q.Create().IsOk() && done.Create().IsOk());
  static Producer p{};
  p.queue = &q;
  p.flags = &done;
  KB_REQUIRE(p.Start(r::ThreadConfig{}).IsOk());
  uint32_t expected = 0U;
  uint32_t got = 0U;
  while ((expected < 100U) && q.ReceiveFor(got, Milliseconds{1000U})) {
    KB_CHECK_EQ(got, expected);
    ++expected;
  }
  KB_CHECK_EQ(expected, 100U);
  KB_CHECK((done.WaitAll(0x1U, Milliseconds{1000U}) & 0x1U) == 0x1U);
  KB_CHECK(p.Join(Milliseconds{1000U}));
  KB_CHECK(p.State() == r::ThreadState::Stopped);
}

KB_TEST("host: a held mutex refuses another thread for at least the timeout") {
  static r::Mutex m{};
  KB_REQUIRE(m.Create().IsOk());
  static Holder h{};
  h.mutex = &m;
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{2U};
  KB_REQUIRE(h.Start(cfg).IsOk());
  while (!h.held.load()) {
    r::SleepFor(Milliseconds{1U});
  }
  const kb::Instant t0 = r::Now();
  KB_CHECK(!m.TryLockFor(Milliseconds{30U}));
  KB_CHECK(kb::utils::ElapsedUs(r::Now(), t0) >= 30000U);
  h.RequestStop();
  KB_CHECK(h.Join(Milliseconds{1000U}));
  KB_CHECK(m.TryLockFor(Milliseconds{100U}));
  m.Unlock();
}

KB_TEST("host: semaphores and flags wake a waiting thread; timers fire") {
  static r::Semaphore s{};
  KB_REQUIRE(s.Create(1U, 0U).IsOk());
  KB_CHECK(!s.TakeFor(Milliseconds{5U}));
  KB_CHECK(s.GiveFromIsr());
  KB_CHECK(s.TakeFor(Milliseconds{5U}));
  static std::atomic<uint32_t> fired{0U};
  static r::Timer t{};
  KB_REQUIRE(
      t.Create([](void* c) noexcept { static_cast<std::atomic<uint32_t>*>(c)->fetch_add(1U); },
               &fired, Milliseconds{5U}, true)
          .IsOk());
  KB_CHECK(t.Start());
  // Wait for three expiries with a generous bound: a loaded machine may delay the timer thread.
  for (uint32_t waited = 0U; (fired.load() < 3U) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_CHECK(t.Stop());
  KB_CHECK(fired.load() >= 3U);
  static r::EventFlags e{};
  KB_REQUIRE(e.Create().IsOk());
  KB_CHECK(e.SetFromIsr(0x10U));
  KB_CHECK_EQ(e.WaitAny(0x30U, Milliseconds{5U}) & 0x10U, 0x10U);
  KB_CHECK_EQ(e.WaitAny(0x30U, Milliseconds{5U}) & 0x30U, 0U);
  e.Set(0x20U);
  e.Clear(0x20U);
  KB_CHECK_EQ(e.Get(), 0U);
  KB_CHECK_EQ(s.Count(), 0U);
}

KB_TEST("host: a periodic thread steps on its grid and stops on request") {
  static CountingThread t{};
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{5U};
  const kb::Instant t0 = r::Now();
  KB_REQUIRE(t.Start(cfg).IsOk());
  for (uint32_t waited = 0U; (t.steps_done.load() < 5U) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  t.RequestStop();
  KB_CHECK(t.Join(Milliseconds{1000U}));
  // At most one step per grid point of the time that really passed (the grid catches up after a
  // late step).
  const uint64_t grid_points = (kb::utils::ElapsedUs(r::Now(), t0) / 5000U) + 2U;
  KB_CHECK(t.steps_done.load() >= 5U);
  KB_CHECK(t.steps_done.load() <= grid_points);
  KB_CHECK_EQ(t.cleanups.load(), 1U);
  {
    const r::CriticalSection a{};
    const r::CriticalSection b{};
  }
  static r::Queue<uint8_t, 1> q{};
  KB_REQUIRE(q.Create().IsOk());
  KB_CHECK(q.SendFromIsr(1U) && !q.TrySend(2U));
  KB_CHECK_EQ(q.Size(), 1U);
}

KB_TEST(
    "host: a recursive mutex nests for its holder and refuses another thread until fully given") {
  static r::RecursiveMutex m{};
  KB_REQUIRE(m.Create().IsOk());
  static RecursiveHolder h{};
  h.mutex = &m;
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{2U};
  KB_REQUIRE(h.Start(cfg).IsOk());
  while (!h.held.load()) {
    r::SleepFor(Milliseconds{1U});
  }
  const kb::Instant t0 = r::Now();
  KB_CHECK(!m.TryLockFor(Milliseconds{20U}));
  KB_CHECK(kb::utils::ElapsedUs(r::Now(), t0) >= 20000U);
  h.RequestStop();
  KB_CHECK(h.Join(Milliseconds{1000U}));
  KB_CHECK(m.TryLockFor(Milliseconds{100U}) && m.TryLock());
  m.Unlock();
  m.Unlock();
}

KB_TEST("host: a verified start reports a good Setup as Ok and a failed one as Fault") {
  static CountingThread good{};
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{1U};
  KB_CHECK(good.StartAndWaitToVerify(cfg, Milliseconds{1000U}).IsOk());
  KB_CHECK((good.State() == r::ThreadState::Running) || (good.State() == r::ThreadState::Stopped));
  const kb::Result<std::size_t> stack = good.StackFreeBytes();
  KB_CHECK(stack.IsOk() || (stack.Error() == Status::Unsupported));
  good.RequestStop();
  KB_CHECK(good.Join(Milliseconds{1000U}));
  static CountingThread bad{};
  bad.setup_ok = false;
  KB_CHECK_EQ(bad.StartAndWaitToVerify(cfg, Milliseconds{1000U}).Error(), Status::Fault);
  KB_CHECK(bad.State() == r::ThreadState::Failed);
  KB_CHECK_EQ(bad.cleanups.load(), 1U);
  KB_CHECK(bad.Join(Milliseconds{1000U}));
}

KB_TEST("host: a parked thread takes no steps until resumed, and a stop ends a park") {
  static CountingThread t{};
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{1U};
  KB_REQUIRE(t.Start(cfg).IsOk());
  for (uint32_t waited = 0U; (t.steps_done.load() < 2U) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  t.RequestPark();
  for (uint32_t waited = 0U; (!t.IsParked()) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_REQUIRE(t.IsParked());
  const uint32_t parked_at = t.steps_done.load();
  const uint32_t overruns_at = t.Stats().overruns;
  r::SleepFor(Milliseconds{30U});
  KB_CHECK_EQ(t.steps_done.load(), parked_at);
  KB_CHECK_EQ(t.Stats().overruns, overruns_at);
  t.Resume();
  for (uint32_t waited = 0U; (t.steps_done.load() < (parked_at + 3U)) && (waited < 2000U);
       ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_CHECK(t.steps_done.load() >= (parked_at + 3U));
  KB_CHECK(!t.IsParked());
  // The grid restarted on resume: the 30 parked periods are not counted as overruns.
  KB_CHECK(t.Stats().overruns < (overruns_at + 10U));
  t.RequestPark();
  for (uint32_t waited = 0U; (!t.IsParked()) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_REQUIRE(t.IsParked());
  const uint32_t parked_again = t.steps_done.load();
  t.ResumeFromIsr();
  for (uint32_t waited = 0U; (t.steps_done.load() < (parked_again + 2U)) && (waited < 2000U);
       ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_CHECK(t.steps_done.load() >= (parked_again + 2U));
  t.RequestPark();
  for (uint32_t waited = 0U; (!t.IsParked()) && (waited < 2000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  t.RequestStop();
  KB_CHECK(t.Join(Milliseconds{1000U}));
  KB_CHECK(t.State() == r::ThreadState::Stopped);
  KB_CHECK_EQ(t.cleanups.load(), 1U);
}

KB_TEST("host: a verified start and a join end within two ticks of their timeout") {
  static SlowSetup slow{};
  const kb::Instant t0 = r::Now();
  KB_CHECK_EQ(slow.StartAndWaitToVerify(r::ThreadConfig{}, Milliseconds{50U}).Error(),
              Status::Timeout);
  const uint64_t verify_us = kb::utils::ElapsedUs(r::Now(), t0);
  KB_CHECK(verify_us >= 50000U);
  KB_CHECK(verify_us < (50000U + (2U * kTickUs) + kSlackUs));
  const kb::Instant t1 = r::Now();
  KB_CHECK(!slow.Join(Milliseconds{50U}));
  const uint64_t join_us = kb::utils::ElapsedUs(r::Now(), t1);
  KB_CHECK(join_us >= 50000U);
  KB_CHECK(join_us < (50000U + (2U * kTickUs) + kSlackUs));
  KB_CHECK(slow.Join(Milliseconds{2000U}));
  KB_CHECK(slow.State() == r::ThreadState::Stopped);
}
