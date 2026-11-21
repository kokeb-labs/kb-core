// The kb::rtos contract in one context, deterministic: built over the none backend and over the
// FreeRTOS and ThreadX boundaries with their fake kernels, so every backend is held to the same
// results and tick counts. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"
#include "rtos_test_clock.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;

namespace {
class SmallThread : public r::Thread<SmallThread, 1024U> {
public:
  static bool Setup() noexcept {
    return true;
  }
  static bool Step() noexcept {
    return false;
  }
  void Cleanup() noexcept {}
};
struct Sample {
  uint16_t id;
  int32_t value;
};
void CountFire(void* context) noexcept {
  ++*static_cast<uint32_t*>(context);
}

// Constructs a T in the same bytes `rounds` times, runs `use` on it and ends it with its
// destructor: each round's RTOS object is created where the last one was deleted (ADR-0017).
template <typename T, typename Use>
bool InSameStorage(uint32_t rounds, Use use) noexcept {
  alignas(T) static std::array<std::byte, sizeof(T)> bytes{};
  bool ok = true;
  for (uint32_t i = 0U; i < rounds; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): placement into the static bytes above
    T* const object = ::new (static_cast<void*>(bytes.data())) T{};
    ok = use(*object) && ok;
    object->~T();
  }
  return ok;
}
} // namespace

KB_TEST("time: ticks cover at least the duration plus one tick") {
  KB_CHECK_EQ(r::TickRateHz(), 1000U);
  KB_CHECK_EQ(r::ToTicksAtLeast(Milliseconds{0U}), 0U);
  KB_CHECK_EQ(r::ToTicksAtLeast(Milliseconds{1U}), 2U);
  KB_CHECK_EQ(r::ToTicksAtLeast(Milliseconds{10U}), 11U);
  KB_CHECK_EQ(r::ToTicksAtLeast(kb::kMaxMilliseconds), UINT32_MAX);
  KB_CHECK_EQ(r::ToTicksAtLeast(Milliseconds{UINT32_MAX - 1U}), UINT32_MAX);
}

KB_TEST("time: SleepFor waits at least the time asked, in both units") {
  kb_rtos_test::ResetBackend();
  const kb::Instant t0 = r::Now();
  const uint32_t tick0 = kb_rtos_test::Tick();
  r::SleepFor(Milliseconds{5U});
  KB_CHECK_EQ(kb_rtos_test::Tick() - tick0, 6U);
  KB_CHECK(kb::utils::ElapsedUs(r::Now(), t0) >= 5000U);
  r::SleepFor(kb::Microseconds{1500U});
  KB_CHECK_EQ(kb_rtos_test::Tick() - tick0, 9U);
  r::SleepFor(Milliseconds{0U});
  r::Yield();
  KB_CHECK_EQ(kb_rtos_test::Tick() - tick0, 9U);
}

KB_TEST("time: Periodic keeps a fixed grid and reports overruns") {
  kb_rtos_test::ResetBackend();
  r::Periodic grid{};
  const uint32_t start = kb_rtos_test::Tick();
  KB_CHECK(grid.Wait(Milliseconds{10U}));
  KB_CHECK_EQ(kb_rtos_test::Tick() - start, 10U);
  kb_rtos_test::Advance(4U); // a 4 ms step
  KB_CHECK(grid.Wait(Milliseconds{10U}));
  KB_CHECK_EQ(kb_rtos_test::Tick() - start, 20U);
  kb_rtos_test::Advance(15U); // a step longer than the period
  KB_CHECK(!grid.Wait(Milliseconds{10U}));
  KB_CHECK_EQ(grid.Overruns(), 1U);
  KB_CHECK(grid.Wait(Milliseconds{10U}));
  KB_CHECK_EQ(kb_rtos_test::Tick() - start, 40U);
  r::Periodic fast{};
  KB_CHECK(fast.Wait(Milliseconds{0U})); // a zero period is one tick
}

KB_TEST("mutex: create once, bounded try-lock fails after at least its timeout") {
  kb_rtos_test::ResetBackend();
  r::Mutex m{};
  KB_CHECK(!m.TryLock());
  KB_CHECK(!m.TryLockFor(Milliseconds{1U}));
  m.Unlock();
  KB_CHECK(!m.IsCreated());
  KB_CHECK(m.Create().IsOk());
  KB_CHECK_EQ(m.Create().Error(), Status::AlreadyStarted);
  KB_CHECK(m.TryLock());
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK(!m.TryLockFor(Milliseconds{3U}));
  KB_CHECK(kb_rtos_test::Tick() - t0 >= 3U);
  m.Unlock();
  {
    r::LockGuard guard{m, Milliseconds{1U}};
    KB_CHECK(guard.OwnsLock());
    r::LockGuard second{m, Milliseconds{0U}};
    KB_CHECK(!second.OwnsLock());
  }
  KB_CHECK(m.TryLockFor(Milliseconds{0U}));
  m.Unlock();
}

KB_TEST("recursive mutex: the holder nests, each level needs its unlock") {
  kb_rtos_test::ResetBackend();
  r::RecursiveMutex m{};
  KB_CHECK(!m.TryLock() && !m.TryLockFor(Milliseconds{1U}) && !m.IsCreated());
  m.Unlock();
  KB_CHECK(m.Create().IsOk());
  KB_CHECK_EQ(m.Create().Error(), Status::AlreadyStarted);
  KB_CHECK(m.IsCreated());
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK(m.TryLock() && m.TryLockFor(Milliseconds{5U}));
  {
    r::RecursiveLockGuard guard{m, Milliseconds{1U}};
    KB_CHECK(guard.OwnsLock());
  }
  KB_CHECK_EQ(kb_rtos_test::Tick(), t0);
  m.Unlock();
  m.Unlock();
  KB_CHECK(m.TryLock());
  m.Unlock();
}

KB_TEST("queue: item buffers are whole queue words per slot") {
  constexpr std::size_t kWord = r::detail::kQueueWordBytes;
  KB_CHECK_EQ(r::detail::QueueBufferBytes(1U, 3U), kWord * 3U);
  KB_CHECK_EQ(r::detail::QueueBufferBytes(kWord, 2U), kWord * 2U);
  KB_CHECK_EQ(r::detail::QueueBufferBytes(kWord + 1U, 1U), kWord * 2U);
}

KB_TEST("semaphore: counts, bounds and arguments") {
  kb_rtos_test::ResetBackend();
  r::Semaphore s{};
  KB_CHECK(!s.Give() && !s.TryTake() && !s.GiveFromIsr() && !s.TakeFor(Milliseconds{1U}));
  KB_CHECK_EQ(s.Count(), 0U);
  KB_CHECK_EQ(s.Create(0U, 0U).Error(), Status::InvalidArgument);
  KB_CHECK_EQ(s.Create(1U, 2U).Error(), Status::InvalidArgument);
  KB_CHECK(s.Create(2U, 1U).IsOk());
  KB_CHECK_EQ(s.Create(2U, 1U).Error(), Status::AlreadyStarted);
  KB_CHECK_EQ(s.Count(), 1U);
  KB_CHECK(s.Give());
  KB_CHECK(!s.Give());
  KB_CHECK(!s.GiveFromIsr());
  KB_CHECK(s.TryTake() && s.TakeFor(Milliseconds{1U}));
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK(!s.TakeFor(Milliseconds{2U}));
  KB_CHECK(kb_rtos_test::Tick() - t0 >= 2U);
  KB_CHECK(s.GiveFromIsr());
  KB_CHECK_EQ(s.Count(), 1U);
}

KB_TEST("queue: FIFO copies, full and empty edges with bounded waits") {
  kb_rtos_test::ResetBackend();
  r::Queue<Sample, 2> q{};
  Sample out{};
  KB_CHECK(!q.TrySend(Sample{1U, 1}) && !q.TryReceive(out) && !q.SendFromIsr(Sample{}));
  KB_CHECK(!q.SendFor(Sample{}, Milliseconds{1U}) && !q.ReceiveFor(out, Milliseconds{1U}));
  KB_CHECK_EQ(q.Size(), 0U);
  KB_CHECK_EQ((r::Queue<Sample, 2>::Capacity()), 2U);
  KB_CHECK(q.Create().IsOk());
  KB_CHECK_EQ(q.Create().Error(), Status::AlreadyStarted);
  KB_CHECK(q.TrySend(Sample{1U, -5}) && q.SendFromIsr(Sample{2U, 7}));
  KB_CHECK_EQ(q.Size(), 2U);
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK(!q.SendFor(Sample{3U, 0}, Milliseconds{4U}));
  KB_CHECK(kb_rtos_test::Tick() - t0 >= 4U);
  KB_CHECK(q.ReceiveFor(out, Milliseconds{1U}) && (out.id == 1U) && (out.value == -5));
  KB_CHECK(q.TryReceive(out) && (out.id == 2U) && (out.value == 7));
  KB_CHECK(!q.TryReceive(out));
  for (uint16_t i = 0U; i < 5U; ++i) {
    KB_CHECK(q.SendFor(Sample{i, 0}, Milliseconds{1U}) && q.TryReceive(out) && (out.id == i));
  }
}

KB_TEST("event flags: any, all, clear on exit and the 24-bit mask") {
  kb_rtos_test::ResetBackend();
  r::EventFlags f{};
  f.Set(1U);
  f.Clear(1U);
  KB_CHECK(!f.SetFromIsr(1U));
  KB_CHECK_EQ(f.Get(), 0U);
  KB_CHECK_EQ(f.WaitAny(1U, Milliseconds{1U}), 0U);
  KB_CHECK(f.Create().IsOk());
  KB_CHECK_EQ(f.Create().Error(), Status::AlreadyStarted);
  f.Set(0xFF000003UL);
  KB_CHECK_EQ(f.Get(), 0x3U);
  KB_CHECK_EQ(f.WaitAny(0x2U, Milliseconds{1U}, false) & 0x2U, 0x2U);
  KB_CHECK_EQ(f.Get(), 0x3U);
  KB_CHECK_EQ(f.WaitAll(0x3U, Milliseconds{1U}) & 0x3U, 0x3U);
  KB_CHECK_EQ(f.Get(), 0U);
  KB_CHECK(f.SetFromIsr(0x4U));
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK_EQ(f.WaitAll(0x6U, Milliseconds{2U}) & 0x6U, 0x4U);
  KB_CHECK(kb_rtos_test::Tick() - t0 >= 2U);
  KB_CHECK_EQ(f.WaitAny(0U, Milliseconds{1U}), 0U);
  f.Clear(r::kAllFlags);
  KB_CHECK_EQ(f.Get(), 0U);
}

KB_TEST("timer: periodic and one-shot callbacks as time advances") {
  kb_rtos_test::ResetBackend();
  static r::Timer periodic{};
  static r::Timer once{};
  static uint32_t once_fired = 0U;
  static uint32_t g_fired = 0U;
  KB_CHECK(!periodic.Start() && !periodic.Stop());
  KB_CHECK_EQ(periodic.Create(nullptr, nullptr, Milliseconds{5U}, true).Error(),
              Status::InvalidArgument);
  KB_CHECK_EQ(periodic.Create(&CountFire, &g_fired, Milliseconds{0U}, true).Error(),
              Status::InvalidArgument);
  KB_CHECK(periodic.Create(&CountFire, &g_fired, Milliseconds{5U}, true).IsOk());
  KB_CHECK_EQ(periodic.Create(&CountFire, &g_fired, Milliseconds{5U}, true).Error(),
              Status::AlreadyStarted);
  KB_CHECK(once.Create(&CountFire, &once_fired, Milliseconds{3U}, false).IsOk());
  KB_CHECK(periodic.Start() && once.Start());
  kb_rtos_test::Advance(12U);
  KB_CHECK_EQ(g_fired, 2U);
  KB_CHECK_EQ(once_fired, 1U);
  KB_CHECK(periodic.Stop());
  kb_rtos_test::Advance(20U);
  KB_CHECK_EQ(g_fired, 2U);
  KB_CHECK_EQ(once_fired, 1U);
}

KB_TEST("critical section: nests and always leaves") {
  {
    const r::CriticalSection outer{};
    const r::CriticalSection inner{};
  }
  KB_CHECK(true);
}

KB_TEST("try calls never wait; edges of counts and capacities") {
  kb_rtos_test::ResetBackend();
  r::Mutex m{};
  KB_REQUIRE(m.Create().IsOk() && m.TryLock());
  const uint32_t t0 = kb_rtos_test::Tick();
  KB_CHECK(!m.TryLock());
  r::Semaphore s{};
  KB_REQUIRE(s.Create(2U, 2U).IsOk());
  KB_CHECK(s.TryTake() && s.TryTake() && !s.TryTake());
  r::Queue<uint8_t, 1> one{};
  KB_REQUIRE(one.Create().IsOk());
  uint8_t x = 0U;
  KB_CHECK(!one.TryReceive(x));
  KB_CHECK(one.TrySend(1U) && !one.TrySend(2U) && !one.SendFromIsr(3U));
  KB_CHECK_EQ(kb_rtos_test::Tick(), t0);
  m.Unlock();
  r::Queue<uint8_t, 3> three{};
  KB_REQUIRE(three.Create().IsOk());
  for (uint8_t round = 0U; round < 4U; ++round) {
    KB_CHECK(three.TrySend(round) && three.TrySend(static_cast<uint8_t>(round + 10U)));
    KB_CHECK(three.TryReceive(x) && (x == round));
    KB_CHECK(three.TryReceive(x) && (x == (round + 10U)));
  }
  r::EventFlags f{};
  KB_REQUIRE(f.Create().IsOk());
  KB_CHECK_EQ(f.Get(), 0U);
  const uint32_t t1 = kb_rtos_test::Tick();
  KB_CHECK_EQ(f.WaitAny(0x8U, Milliseconds{3U}), 0U);
  KB_CHECK(kb_rtos_test::Tick() - t1 >= 3U);
}

KB_TEST("time: a step of exactly one period is an overrun (the wake-up tick has come)") {
  kb_rtos_test::ResetBackend();
  r::Periodic grid{};
  KB_CHECK(grid.Wait(Milliseconds{10U}));
  kb_rtos_test::Advance(10U);
  KB_CHECK(!grid.Wait(Milliseconds{10U}));
  KB_CHECK_EQ(grid.Overruns(), 1U);
}

KB_TEST("thread defaults: state values, no stop requested, the smallest stack") {
  KB_CHECK_EQ(static_cast<uint8_t>(r::ThreadState::Idle), 0U);
  KB_CHECK_EQ(static_cast<uint8_t>(r::ThreadState::Starting), 1U);
  KB_CHECK_EQ(static_cast<uint8_t>(r::ThreadState::Running), 2U);
  KB_CHECK_EQ(static_cast<uint8_t>(r::ThreadState::Stopped), 3U);
  KB_CHECK_EQ(static_cast<uint8_t>(r::ThreadState::Failed), 4U);
  KB_CHECK_EQ(r::ThreadConfig{}.priority, 1U);
  static SmallThread small{};
  KB_CHECK(!small.StopRequested());
  KB_CHECK_EQ(small.Stats().max_step_us, 0U);
}

KB_TEST("lifetime: each destructor deletes its RTOS object; the next one in the same bytes works") {
  kb_rtos_test::ResetBackend();
  constexpr uint32_t kRounds = 8U;
  using SampleQueue = r::Queue<Sample, 2>;
  const bool mutexes = InSameStorage<r::Mutex>(kRounds, [](r::Mutex& m) noexcept {
    const bool ok = m.Create().IsOk() && m.TryLock() && (!m.TryLockFor(Milliseconds{0U}));
    m.Unlock();
    return ok;
  });
  const bool recursive =
      InSameStorage<r::RecursiveMutex>(kRounds, [](r::RecursiveMutex& m) noexcept {
        const bool ok = m.Create().IsOk() && m.TryLock() && m.TryLock();
        m.Unlock();
        m.Unlock();
        return ok;
      });
  const bool semaphores = InSameStorage<r::Semaphore>(kRounds, [](r::Semaphore& s) noexcept {
    return s.Create(2U, 0U).IsOk() && (s.Count() == 0U) && s.Give() && s.TryTake() && !s.TryTake();
  });
  const bool queues = InSameStorage<SampleQueue>(kRounds, [](SampleQueue& q) noexcept {
    Sample out{};
    return q.Create().IsOk() && (q.Size() == 0U) && q.TrySend(Sample{.id = 7U, .value = 1}) &&
           q.TryReceive(out) && (out.id == 7U);
  });
  const bool groups = InSameStorage<r::EventFlags>(kRounds, [](r::EventFlags& f) noexcept {
    const bool fresh = f.Create().IsOk() && (f.Get() == 0U);
    f.Set(0x5U);
    return fresh && ((f.WaitAll(0x5U, Milliseconds{0U}) & 0x5U) == 0x5U);
  });
  // Never created: the destructor has nothing to delete.
  const auto untouched = [](const auto&) noexcept { return true; };
  const bool idle =
      InSameStorage<r::Timer>(1U, untouched) && InSameStorage<r::Mutex>(1U, untouched) &&
      InSameStorage<r::RecursiveMutex>(1U, untouched) &&
      InSameStorage<r::Semaphore>(1U, untouched) && InSameStorage<SampleQueue>(1U, untouched) &&
      InSameStorage<r::EventFlags>(1U, untouched);
  KB_CHECK(mutexes && recursive && semaphores && queues && groups && idle);
}
