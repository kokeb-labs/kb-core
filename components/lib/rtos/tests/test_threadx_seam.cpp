// Link-seam tests of the ThreadX boundary file over the fake kernel: creation results, the thread
// parameters ThreadX gets, the thread life cycle run to its end, the rules ThreadX does not have
// (non-recursive mutex, semaphore maximum, delay-until), timers, critical sections and stack
// high-water. The real kernel runs the threaded suite (test_rtos_host.cpp,
// test_threadx_kernel.cpp). SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "counting_thread.hpp"
#include "fake_threadx.hpp"
#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;
namespace b = kb::rtos::backend;
namespace f = kb_fake_threadx;

namespace {
void Nothing(void* context) noexcept {
  static_cast<void>(context);
}
void Count(void* context) noexcept {
  ++*static_cast<uint32_t*>(context);
}
struct Big {
  // cppcheck-suppress unusedStructMember ; only its size matters
  std::array<uint8_t, 65> bytes;
};
struct Odd {
  uint8_t a;
  uint16_t b;
  uint8_t c;
};
} // namespace

KB_TEST("threadx seam: a failed create is a Fault and leaves the object unusable") {
  f::Reset();
  r::Mutex m{};
  f::FailNextCall();
  KB_CHECK_EQ(m.Create().Error(), Status::Fault);
  KB_CHECK(!m.IsCreated() && !m.TryLock());
  KB_CHECK(m.Create().IsOk());
  r::RecursiveMutex rm{};
  f::FailNextCall();
  KB_CHECK_EQ(rm.Create().Error(), Status::Fault);
  r::Semaphore s{};
  f::FailNextCall();
  KB_CHECK_EQ(s.Create(1U, 0U).Error(), Status::Fault);
  r::Queue<uint32_t, 4> q{};
  f::FailNextCall();
  KB_CHECK_EQ(q.Create().Error(), Status::Fault);
  r::EventFlags e{};
  f::FailNextCall();
  KB_CHECK_EQ(e.Create().Error(), Status::Fault);
  r::detail::TimerStorage ts{};
  r::detail::TimerParams tp{};
  tp.callback = &Nothing;
  f::FailNextCall();
  KB_CHECK_EQ(b::TimerCreate(ts, tp), Status::Fault);
  KB_CHECK(ts.handle == nullptr);
  KB_CHECK_EQ(b::TimerCreate(ts, tp), Status::Ok); // the failed one gave its table slot back
}

KB_TEST("threadx seam: arguments are checked before the kernel is called") {
  f::Reset();
  r::detail::QueueStorage storage{};
  alignas(r::detail::kStorageAlign) std::array<std::byte, 160> buffer{};
  KB_CHECK_EQ(b::QueueCreate(storage, nullptr, 4U, 2U), Status::InvalidArgument);
  KB_CHECK_EQ(b::QueueCreate(storage, buffer.data(), 0U, 2U), Status::InvalidArgument);
  KB_CHECK_EQ(b::QueueCreate(storage, buffer.data(), 4U, 0U), Status::InvalidArgument);
  KB_CHECK(storage.handle == nullptr);
  KB_CHECK_EQ(b::QueueCreate(storage, buffer.data(), 64U, 2U), Status::Ok);
  r::Queue<Big, 1> big{};
  KB_CHECK_EQ(big.Create().Error(), Status::Unsupported); // above 16 words
  r::detail::SemaphoreStorage sem{};
  KB_CHECK_EQ(b::SemaphoreCreate(sem, 0U, 0U), Status::InvalidArgument);
  KB_CHECK_EQ(b::SemaphoreCreate(sem, 1U, 2U), Status::InvalidArgument);
  KB_CHECK(sem.handle == nullptr);
  KB_CHECK_EQ(b::SemaphoreCreate(sem, 1U, 1U), Status::Ok);
}

KB_TEST("threadx seam: a queue carries items that are not whole words, in order") {
  f::Reset();
  r::Queue<Odd, 3> q{};
  KB_REQUIRE(q.Create().IsOk());
  KB_CHECK(q.TrySend(Odd{1U, 0xBEEFU, 2U}) && q.SendFromIsr(Odd{3U, 0x1234U, 4U}));
  KB_CHECK_EQ(q.Size(), 2U);
  Odd out{};
  KB_CHECK(q.TryReceive(out) && (out.a == 1U) && (out.b == 0xBEEFU) && (out.c == 2U));
  KB_CHECK(q.ReceiveFor(out, Milliseconds{1U}) && (out.a == 3U) && (out.b == 0x1234U));
  KB_CHECK(!q.TryReceive(out));
  KB_CHECK(q.TrySend(Odd{}) && q.TrySend(Odd{}) && q.TrySend(Odd{}));
  const uint32_t t0 = b::TickCount();
  KB_CHECK(!q.SendFromIsr(Odd{}));
  KB_CHECK_EQ(b::TickCount(), t0);
}

KB_TEST("threadx seam: a thread gets its stack, a copied name and a mapped, clamped priority") {
  f::Reset();
  static CountingThread a{};
  static CountingThread c{};
  r::ThreadConfig cfg{};
  cfg.name = "a-thread-name-longer-than-sixteen";
  cfg.priority = 3U;
  KB_CHECK(a.Start(cfg).IsOk());
  KB_CHECK_EQ(a.Start(cfg).Error(), Status::AlreadyStarted);
  cfg.name = "short";
  cfg.priority = 1000U;
  KB_CHECK(c.Start(cfg).IsOk());
  KB_REQUIRE(f::ThreadCount() == 2U);
  TX_THREAD* t0 = f::Thread(0U);
  TX_THREAD* t1 = f::Thread(1U);
  KB_CHECK_EQ(t0->tx_thread_stack_size, 4096U);
  KB_CHECK_EQ(t0->priority, 28U); // 31 - 3: ThreadX runs 0 first
  KB_CHECK_EQ(t0->preempt_threshold, 28U);
  KB_CHECK_EQ(t0->time_slice, TX_NO_TIME_SLICE);
  KB_CHECK_EQ(t0->auto_start, TX_AUTO_START);
  KB_CHECK_EQ(t0->entry_input, 0U);
  KB_CHECK_EQ(t1->priority, 1U); // never the timer thread's 0
  KB_REQUIRE(t0->name != nullptr);
  KB_CHECK_EQ(t0->name[14], 'l');
  KB_CHECK_EQ(t0->name[15], '\0');
  KB_CHECK_EQ(t1->name[5], '\0');
  KB_CHECK_EQ(t1->name[0], 's');
}

KB_TEST("threadx seam: thread creation refuses a missing body, a missing or too small stack") {
  f::Reset();
  r::detail::ThreadStorage storage{};
  r::detail::ThreadCore no_run{};
  alignas(r::detail::kStackAlign) static std::array<std::byte, 256> stack{};
  r::detail::ThreadParams p{};
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::InvalidArgument);
  p.core = &no_run;
  p.stack = stack.data();
  p.stack_bytes = stack.size();
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::InvalidArgument);
  r::detail::ThreadCore runs{};
  runs.run = [](r::detail::ThreadCore&) noexcept {};
  p.core = &runs;
  p.stack = nullptr;
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::InvalidArgument);
  p.stack = stack.data();
  p.stack_bytes = 199U; // below TX_MINIMUM_STACK
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::InvalidArgument);
  p.stack_bytes = 200U;
  p.name = nullptr;
  f::FailNextCall();
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::Fault);
  KB_CHECK(storage.handle == nullptr);
  KB_CHECK_EQ(b::ThreadCreate(storage, p), Status::Ok);
  KB_CHECK_EQ(f::Thread(0U)->name[0], 'k');
  b::ThreadExit(storage);
  b::ThreadRelease(storage);
  KB_CHECK(storage.handle == nullptr);
  KB_CHECK_EQ(f::Terminates(), 1U);
  KB_CHECK_EQ(f::Deletes(), 1U);
  b::ThreadRelease(storage);
  KB_CHECK_EQ(f::Deletes(), 1U);
}

KB_TEST("threadx seam: a thread runs Setup, its steps on the period grid and Cleanup") {
  f::Reset();
  static CountingThread t{};
  t.limit = 3U;
  t.step_ticks = 2U;
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{10U};
  KB_REQUIRE(t.Start(cfg).IsOk());
  const uint32_t start = b::TickCount();
  f::RunThread(0U);
  KB_CHECK_EQ(t.setups.load(), 1U);
  KB_CHECK_EQ(t.steps_done.load(), 3U);
  KB_CHECK_EQ(t.cleanups.load(), 1U);
  KB_CHECK(t.State() == r::ThreadState::Stopped);
  KB_CHECK_EQ(t.Stats().overruns, 0U);
  KB_CHECK(t.Stats().max_step_us >= 2000U);
  KB_CHECK_EQ(b::TickCount() - start, 22U);
  static CountingThread slow{};
  slow.limit = 3U;
  slow.step_ticks = 15U;
  KB_REQUIRE(slow.Start(cfg).IsOk());
  f::RunThread(1U);
  KB_CHECK_EQ(slow.Stats().overruns, 2U);
  static CountingThread spin{};
  spin.limit = 4U;
  KB_REQUIRE(spin.Start(r::ThreadConfig{}).IsOk());
  const uint32_t before = f::Relinquishes();
  f::RunThread(2U);
  KB_CHECK_EQ(f::Relinquishes() - before, 3U);
}

KB_TEST("threadx seam: a thread creates its park semaphore once; a failed one fails the start") {
  f::Reset();
  static CountingThread t{};
  f::FailNextCall(); // the park semaphore, created first
  KB_CHECK_EQ(t.Start(r::ThreadConfig{}).Error(), Status::Fault);
  KB_CHECK(t.State() == r::ThreadState::Idle);
  KB_CHECK_EQ(f::ThreadCount(), 0U);
  f::FailNextThreadCreate(); // the semaphore is made, the thread is not
  KB_CHECK_EQ(t.Start(r::ThreadConfig{}).Error(), Status::Fault);
  KB_CHECK(t.State() == r::ThreadState::Idle);
  KB_CHECK(t.Start(r::ThreadConfig{}).IsOk()); // the retry keeps the semaphore it made
  KB_CHECK_EQ(f::SemaphoreCreates(), 1U);
  KB_CHECK_EQ(f::ThreadCount(), 1U);
}

KB_TEST("threadx seam: a recursive try-lock held by another thread never waits") {
  f::Reset();
  r::RecursiveMutex m{};
  KB_REQUIRE(m.Create().IsOk() && m.TryLock());
  TX_THREAD other{};
  f::SetCurrent(&other);
  const uint32_t t0 = b::TickCount();
  KB_CHECK(!m.TryLock());
  KB_CHECK_EQ(b::TickCount(), t0);
  f::SetCurrent(f::TestThread());
  m.Unlock();
}

KB_TEST("threadx seam: a verified start fails closed when Setup does not return in time") {
  f::Reset();
  static CountingThread t{};
  const uint32_t t0 = b::TickCount();
  KB_CHECK_EQ(t.StartAndWaitToVerify(r::ThreadConfig{}, Milliseconds{5U}).Error(), Status::Timeout);
  KB_CHECK_EQ(b::TickCount() - t0, 5U); // polled once a tick until 5 ms had passed: the timeout
  static CountingThread again{};
  KB_REQUIRE(again.Start(r::ThreadConfig{}).IsOk());
  KB_CHECK_EQ(again.StartAndWaitToVerify(r::ThreadConfig{}, Milliseconds{1U}).Error(),
              Status::AlreadyStarted);
}

KB_TEST("threadx seam: the holder of a kb::Mutex cannot take it again; others wait their timeout") {
  f::Reset();
  r::Mutex m{};
  KB_REQUIRE(m.Create().IsOk());
  KB_CHECK(m.TryLock());
  uint32_t t0 = b::TickCount();
  const uint32_t sleeps = f::Sleeps();
  KB_CHECK(!m.TryLock());
  KB_CHECK_EQ(b::TickCount(), t0);
  KB_CHECK_EQ(f::Sleeps(), sleeps);
  KB_CHECK(!m.TryLockFor(Milliseconds{3U}));
  KB_CHECK_EQ(b::TickCount() - t0, 4U);
  t0 = b::TickCount();
  TX_THREAD other{};
  f::SetCurrent(&other);
  KB_CHECK(!m.TryLockFor(Milliseconds{2U}));
  KB_CHECK_EQ(b::TickCount() - t0, 3U);
  m.Unlock(); // not the holder: refused by the kernel
  f::SetCurrent(f::TestThread());
  m.Unlock();
  f::SetCurrent(&other);
  KB_CHECK(m.TryLock());
  m.Unlock();
  f::SetCurrent(nullptr); // outside a thread there is no holder to compare
  KB_CHECK(m.TryLock());
  f::SetCurrent(f::TestThread());
}

KB_TEST("threadx seam: an unbounded re-take by the holder fails at once instead of hanging") {
  f::Reset();
  r::detail::MutexStorage storage{};
  KB_REQUIRE(b::MutexCreate(storage) == Status::Ok);
  KB_CHECK(b::MutexLock(storage, 0U));
  const uint32_t t0 = b::TickCount();
  const uint32_t sleeps = f::Sleeps();
  KB_CHECK(!b::MutexLock(storage, UINT32_MAX));
  KB_CHECK_EQ(b::TickCount(), t0);
  KB_CHECK_EQ(f::Sleeps(), sleeps);
  b::MutexUnlock(storage);
  r::detail::MutexStorage rec{};
  KB_REQUIRE(b::RecursiveMutexCreate(rec) == Status::Ok);
  KB_CHECK(b::RecursiveMutexLock(rec, UINT32_MAX) && b::RecursiveMutexLock(rec, 0U));
  TX_THREAD other{};
  f::SetCurrent(&other);
  KB_CHECK(!b::RecursiveMutexLock(rec, 2U));
  f::SetCurrent(f::TestThread());
  b::RecursiveMutexUnlock(rec);
  b::RecursiveMutexUnlock(rec);
  f::SetCurrent(&other);
  KB_CHECK(b::RecursiveMutexLock(rec, 0U));
  f::SetCurrent(f::TestThread());
}

KB_TEST("threadx seam: a semaphore keeps its maximum; event waits report the flags") {
  f::Reset();
  r::Semaphore s{};
  KB_REQUIRE(s.Create(2U, 2U).IsOk());
  KB_CHECK(!s.Give() && !s.GiveFromIsr());
  KB_CHECK(s.TryTake());
  KB_CHECK(s.GiveFromIsr());
  KB_CHECK_EQ(s.Count(), 2U);
  r::EventFlags e{};
  KB_REQUIRE(e.Create().IsOk());
  e.Set(0x5U);
  KB_CHECK_EQ(e.WaitAll(0x5U, Milliseconds{1U}, false), 0x5U);
  KB_CHECK_EQ(e.WaitAny(0x4U, Milliseconds{1U}), 0x5U); // flags before the clear
  KB_CHECK_EQ(e.Get(), 0x1U);
  const uint32_t t0 = b::TickCount();
  KB_CHECK_EQ(e.WaitAll(0x3U, Milliseconds{2U}), 0x1U); // timeout: the flags now
  KB_CHECK_EQ(b::TickCount() - t0, 3U);
  KB_CHECK(e.SetFromIsr(0x2U));
  KB_CHECK_EQ(e.WaitAll(0x3U, Milliseconds{1U}), 0x3U);
  KB_CHECK_EQ(e.Get(), 0U);
}

KB_TEST("threadx seam: timers run their callback, restart on start and stop for good") {
  f::Reset();
  static uint32_t fired = 0U;
  fired = 0U;
  static r::Timer t{};
  KB_REQUIRE(t.Create(&Count, &fired, Milliseconds{3U}, true).IsOk());
  KB_REQUIRE(f::TimerCount() == 1U);
  KB_CHECK(!f::Timer(0U)->active);
  KB_CHECK(t.Start());
  f::AdvanceTicks(2U);
  KB_CHECK(t.Start()); // restart: three more ticks
  f::AdvanceTicks(2U);
  KB_CHECK_EQ(fired, 0U);
  f::AdvanceTicks(1U);
  KB_CHECK_EQ(fired, 1U);
  f::AdvanceTicks(6U);
  KB_CHECK_EQ(fired, 3U);
  KB_CHECK(t.Stop());
  f::AdvanceTicks(10U);
  KB_CHECK_EQ(fired, 3U);
  f::FailNextCall();
  KB_CHECK(!t.Start());
  f::FailNextActivate(); // the period was changed, the activation failed
  KB_CHECK(!t.Start());
  static r::Timer once{};
  static uint32_t once_fired = 0U;
  once_fired = 0U;
  KB_REQUIRE(once.Create(&Count, &once_fired, Milliseconds{1U}, false).IsOk());
  KB_CHECK_EQ(f::Timer(1U)->reschedule, 0U);
  KB_CHECK(once.Start());
  f::AdvanceTicks(5U);
  KB_CHECK_EQ(once_fired, 1U);
  f::Timer(0U)->function(1000U); // an unknown slot is ignored
  KB_CHECK_EQ(fired, 3U);
  r::detail::TimerStorage zero{};
  r::detail::TimerParams p{};
  p.callback = &Count;
  p.context = &once_fired;
  p.period_ms = 0U;
  KB_REQUIRE(b::TimerCreate(zero, p) == Status::Ok);
  KB_CHECK(b::TimerStart(zero));
  KB_CHECK_EQ(f::Timer(f::TimerCount() - 1U)->remaining, 1U);
}

KB_TEST("threadx seam: the timer table is bounded; a full table refuses with Full") {
  f::Reset();
  static std::array<r::detail::TimerStorage, 33> timers{};
  static uint32_t fill_fired = 0U;
  r::detail::TimerParams p{};
  p.callback = &Count;
  p.context = &fill_fired;
  p.period_ms = 1U;
  // The earlier cases of this executable hold slots: one per timer the kernel created and the
  // backend did not delete (a failed create gives its slot back). The table holds exactly 32 in
  // all.
  const uint32_t held = f::TimersHeld();
  std::size_t made = 0U;
  Status last = Status::Ok;
  while ((made < timers.size()) && (last == Status::Ok)) {
    last = b::TimerCreate(timers[made], p);
    ++made;
  }
  KB_CHECK_EQ(last, Status::Full);
  KB_CHECK_EQ(held + static_cast<uint32_t>(made) - 1U, 32U); // the last call was refused
  KB_CHECK(timers[made - 1U].handle == nullptr);
  fill_fired = 0U;
  // One past the last slot is ignored (a 33rd slot would run a fill timer).
  f::Timer(0U)->function(32U);
  KB_CHECK_EQ(fill_fired, 0U);
  f::Timer(0U)->function(31U); // the last slot holds a fill timer
  KB_CHECK_EQ(fill_fired, 1U);
  for (std::size_t i = 0U; (i + 1U) < made; ++i) {
    b::TimerDelete(timers[i]); // their slots are free again
  }
  KB_CHECK_EQ(f::TimersHeld(), held);
}

KB_TEST("threadx seam: critical sections nest and restore the outer posture") {
  f::Reset();
  {
    const r::CriticalSection outer{};
    KB_CHECK_EQ(f::Posture(), TX_INT_DISABLE);
    {
      const r::CriticalSection inner{};
    }
    KB_CHECK_EQ(f::Posture(), TX_INT_DISABLE);
  }
  KB_CHECK_EQ(f::Posture(), TX_INT_ENABLE);
  b::ExitCritical(); // unbalanced: ignored
  KB_CHECK_EQ(f::Posture(), TX_INT_ENABLE);
  {
    const r::CriticalSection after{};
    KB_CHECK_EQ(f::Posture(), TX_INT_DISABLE);
  }
  KB_CHECK_EQ(f::Posture(), TX_INT_ENABLE);
  static_cast<void>(tx_interrupt_control(TX_INT_DISABLE));
  {
    const r::CriticalSection nested_in_isr{};
  }
  KB_CHECK_EQ(f::Posture(), TX_INT_DISABLE); // entered disabled, left disabled
  static_cast<void>(tx_interrupt_control(TX_INT_ENABLE));
}

KB_TEST("threadx seam: the clock extends past a tick wrap; sleeps, yields and delay-until") {
  f::Reset();
  f::SetTick(0xFFFFFFF0UL);
  const kb::Instant before = r::Now();
  f::AdvanceTicks(0x20U);
  KB_CHECK_EQ(kb::utils::ElapsedUs(r::Now(), before), 32000U);
  KB_CHECK_EQ(b::TickRateHz(), 1000U);
  const kb::Instant same = r::Now();
  KB_CHECK(r::Now() == same);
  f::AdvanceTicks(1000U);
  KB_CHECK_EQ(kb::utils::ElapsedUs(r::Now(), same), 1000000U);
  const uint32_t relinquishes = f::Relinquishes();
  b::SleepTicks(0U);
  b::Yield();
  KB_CHECK_EQ(f::Relinquishes() - relinquishes, 2U);
  const uint32_t t0 = b::TickCount();
  b::SleepTicks(3U);
  KB_CHECK_EQ(b::TickCount() - t0, 3U);
  uint32_t wake = b::TickCount();
  f::AdvanceTicks(4U);
  KB_CHECK(b::SleepUntilTick(wake, 10U));
  KB_CHECK_EQ(b::TickCount(), wake);
  f::AdvanceTicks(12U);
  const uint32_t late = b::TickCount();
  KB_CHECK(!b::SleepUntilTick(wake, 10U)); // the next point had passed: no sleep
  KB_CHECK_EQ(b::TickCount(), late);
  uint32_t exact = b::TickCount();
  f::AdvanceTicks(10U);
  const uint32_t sleeps = f::Sleeps();
  KB_CHECK(!b::SleepUntilTick(exact, 10U)); // exactly one period: the wake-up tick has come
  KB_CHECK_EQ(f::Sleeps(), sleeps);
  // Preempted between reading the tick and sleeping: the wake-up comes after the grid point.
  uint32_t preempted = b::TickCount();
  f::AdvanceTicks(4U);
  f::SetSleepOvershoot(3U);
  KB_CHECK(!b::SleepUntilTick(preempted, 10U));
  KB_CHECK_EQ(b::TickCount() - (preempted - 10U), 13U);
  KB_CHECK(b::SleepUntilTick(preempted, 10U)); // the grid is kept: the next point is on time
}

KB_TEST("threadx seam: stack high-water is the deepest use the fill pattern shows") {
  f::Reset();
  r::detail::ThreadStorage storage{};
  std::size_t free_bytes = 1U;
  KB_CHECK_EQ(b::ThreadStackFree(storage, free_bytes), Status::NotReady);
  KB_CHECK_EQ(free_bytes, 0U);
  r::detail::ThreadCore runs{};
  runs.run = [](r::detail::ThreadCore&) noexcept {};
  alignas(r::detail::kStackAlign) static std::array<ULONG, 256> stack{};
  r::detail::ThreadParams p{};
  p.core = &runs;
  void* const raw = stack.data();
  p.stack = static_cast<std::byte*>(raw);
  p.stack_bytes = sizeof(stack);
  KB_REQUIRE(b::ThreadCreate(storage, p) == Status::Ok);
  KB_CHECK_EQ(b::ThreadStackFree(storage, free_bytes), Status::Ok);
  KB_CHECK_EQ(free_bytes, sizeof(stack) - 64U); // the initial frame
  stack[100] = 0U;                              // a deeper use than the kernel has recorded
  KB_CHECK_EQ(b::ThreadStackFree(storage, free_bytes), Status::Ok);
  KB_CHECK_EQ(free_bytes, 100U * sizeof(ULONG));
  f::Thread(0U)->tx_thread_stack_highest_ptr = &stack[10]; // the kernel saw deeper still
  KB_CHECK_EQ(b::ThreadStackFree(storage, free_bytes), Status::Ok);
  KB_CHECK_EQ(free_bytes, 10U * sizeof(ULONG));
  stack[0] = 0U; // the lowest word written: nothing is free
  KB_CHECK_EQ(b::ThreadStackFree(storage, free_bytes), Status::Ok);
  KB_CHECK_EQ(free_bytes, 0U);
  static CountingThread t{};
  KB_CHECK_EQ(t.StackFreeBytes().Error(), Status::NotReady);
  t.limit = 1U;
  KB_REQUIRE(t.Start(r::ThreadConfig{}).IsOk());
  KB_CHECK(t.StackFreeBytes().IsOk());
  KB_CHECK(t.StackFreeBytes().Value() <= 4096U);
  f::RunThread(1U);
  KB_CHECK(t.State() == r::ThreadState::Stopped);
  KB_CHECK_EQ(t.StackFreeBytes().Error(), Status::NotReady);
  // A stack with a fill-pattern word right after it: the scan stops at the stack's end.
  struct Guarded {
    std::array<ULONG, 64> words;
    ULONG guard;
  };
  alignas(r::detail::kStackAlign) static Guarded g{};
  r::detail::ThreadStorage whole{};
  void* const guarded = g.words.data();
  p.stack = static_cast<std::byte*>(guarded);
  p.stack_bytes = sizeof(g.words);
  KB_REQUIRE(b::ThreadCreate(whole, p) == Status::Ok);
  g.words.fill(TX_STACK_FILL);
  g.guard = TX_STACK_FILL;
  f::Thread(2U)->tx_thread_stack_highest_ptr = &g.guard + 1; // past the guard
  KB_CHECK_EQ(b::ThreadStackFree(whole, free_bytes), Status::Ok);
  KB_CHECK_EQ(free_bytes, sizeof(g.words));
}

KB_TEST("threadx seam: each destructor deletes its kernel object; a deleted timer frees its slot") {
  f::Reset();
  static uint32_t fired = 0U;
  const uint32_t held = f::TimersHeld();
  {
    r::Mutex m{};
    r::RecursiveMutex rm{};
    r::Semaphore s{};
    r::Queue<uint32_t, 2> q{};
    r::EventFlags g{};
    r::Timer t{};
    KB_REQUIRE(m.Create().IsOk() && rm.Create().IsOk() && s.Create(1U, 0U).IsOk());
    KB_REQUIRE(q.Create().IsOk() && g.Create().IsOk());
    KB_REQUIRE(t.Create(&Count, &fired, Milliseconds{1U}, true).IsOk());
    KB_CHECK(t.Start());
    KB_CHECK_EQ(f::TimersHeld(), held + 1U);
    f::AdvanceTicks(2U);
    KB_CHECK_EQ(fired, 2U);
    KB_CHECK_EQ(f::ObjectDeletes(), 0U);
  }
  KB_CHECK_EQ(f::ObjectDeletes(), 6U);
  KB_CHECK_EQ(f::TimersHeld(), held);
  KB_CHECK(f::Timer(f::TimerCount() - 1U) == nullptr);
  f::AdvanceTicks(3U);
  KB_CHECK_EQ(fired, 2U); // the deleted timer never runs again
}
