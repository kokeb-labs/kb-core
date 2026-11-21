// Link-seam tests of the FreeRTOS boundary files over the fake kernel: creation results, task
// parameters, the thread life cycle run to its end, ISR context switches and critical sections.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "counting_thread.hpp"
#include "fake_freertos.hpp"
#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;
namespace f = kb_fake_freertos;

namespace {
void Nothing(void* context) noexcept {
  static_cast<void>(context);
}
} // namespace

KB_TEST("freertos seam: a failed static create is a Fault and leaves the object unusable") {
  f::Reset();
  r::Mutex m{};
  f::FailNextCreate();
  KB_CHECK_EQ(m.Create().Error(), Status::Fault);
  KB_CHECK(!m.IsCreated() && !m.TryLock());
  KB_CHECK(m.Create().IsOk());
  r::Semaphore s{};
  f::FailNextCreate();
  KB_CHECK_EQ(s.Create(1U, 0U).Error(), Status::Fault);
  r::Queue<uint32_t, 4> q{};
  f::FailNextCreate();
  KB_CHECK_EQ(q.Create().Error(), Status::Fault);
  r::EventFlags e{};
  f::FailNextCreate();
  KB_CHECK_EQ(e.Create().Error(), Status::Fault);
  static r::Timer t{};
  f::FailNextCreate();
  KB_CHECK_EQ(t.Create(&Nothing, nullptr, Milliseconds{1U}, true).Error(), Status::Fault);
}

KB_TEST("freertos seam: queue arguments are checked before the kernel is called") {
  f::Reset();
  kb::rtos::detail::QueueStorage storage{};
  std::array<std::byte, 8> buffer{};
  KB_CHECK_EQ(kb::rtos::backend::QueueCreate(storage, nullptr, 4U, 2U), Status::InvalidArgument);
  KB_CHECK_EQ(kb::rtos::backend::QueueCreate(storage, buffer.data(), 0U, 2U),
              Status::InvalidArgument);
  KB_CHECK_EQ(kb::rtos::backend::QueueCreate(storage, buffer.data(), 4U, 0U),
              Status::InvalidArgument);
  KB_CHECK_EQ(kb::rtos::backend::QueueCreate(storage, buffer.data(), 4U, 2U), Status::Ok);
}

KB_TEST("freertos seam: a thread is created with its stack depth, name and a clamped priority") {
  f::Reset();
  static CountingThread a{};
  static CountingThread b{};
  r::ThreadConfig cfg{};
  cfg.name = "worker";
  cfg.priority = 3U;
  KB_CHECK(a.Start(cfg).IsOk());
  KB_CHECK_EQ(a.Start(cfg).Error(), Status::AlreadyStarted);
  KB_CHECK(a.State() == r::ThreadState::Starting);
  cfg.priority = 1000U;
  KB_CHECK(b.Start(cfg).IsOk());
  KB_REQUIRE(f::TaskCount() == 2U);
  KB_CHECK_EQ(f::Task(0U).depth, 4096U / sizeof(StackType_t));
  KB_CHECK_EQ(f::Task(0U).priority, 3U);
  KB_CHECK_EQ(f::Task(1U).priority, 24U);
  KB_CHECK(f::Task(0U).name != nullptr);
  r::ThreadConfig bad{};
  bad.name = nullptr;
  static CountingThread c{};
  KB_CHECK_EQ(c.Start(bad).Error(), Status::InvalidArgument);
  f::FailNextCreate();
  KB_CHECK_EQ(c.Start(cfg).Error(), Status::Fault);
  KB_CHECK(c.State() == r::ThreadState::Idle);
  kb::rtos::detail::ThreadStorage storage{};
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, kb::rtos::detail::ThreadParams{}),
              Status::InvalidArgument);
}

KB_TEST("freertos seam: a thread runs Setup, its steps on the period grid, Cleanup, then deletes "
        "itself") {
  f::Reset();
  static CountingThread t{};
  t.limit = 3U;
  t.step_ticks = 2U;
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{10U};
  KB_REQUIRE(t.Start(cfg).IsOk());
  const uint32_t start = r::backend::TickCount();
  f::RunTask(0U);
  KB_CHECK_EQ(t.setups.load(), 1U);
  KB_CHECK_EQ(t.steps_done.load(), 3U);
  KB_CHECK_EQ(t.cleanups.load(), 1U);
  KB_CHECK(t.State() == r::ThreadState::Stopped);
  KB_CHECK_EQ(t.Stats().steps, 3U);
  KB_CHECK_EQ(t.Stats().overruns, 0U);
  KB_CHECK(t.Stats().max_step_us >= 2000U);
  KB_CHECK_EQ(r::backend::TickCount() - start, 22U);
  KB_CHECK_EQ(f::TaskDeletes(), 1U);
  KB_CHECK(t.Join(Milliseconds{0U}));
}

KB_TEST("freertos seam: a failed Setup ends Failed, Cleanup still runs; overruns and stops are "
        "counted") {
  f::Reset();
  static CountingThread failing{};
  failing.setup_ok = false;
  KB_REQUIRE(failing.Start(r::ThreadConfig{}).IsOk());
  f::RunTask(0U);
  KB_CHECK(failing.State() == r::ThreadState::Failed);
  KB_CHECK_EQ(failing.steps_done.load(), 0U);
  KB_CHECK_EQ(failing.cleanups.load(), 1U);
  static CountingThread slow{};
  slow.limit = 3U;
  slow.step_ticks = 15U;
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{10U};
  KB_REQUIRE(slow.Start(cfg).IsOk());
  f::RunTask(1U);
  KB_CHECK_EQ(slow.Stats().overruns, 2U);
  static CountingThread spin{};
  spin.limit = 4U;
  KB_REQUIRE(spin.Start(r::ThreadConfig{}).IsOk());
  const uint32_t yields = f::Yields();
  f::RunTask(2U);
  KB_CHECK_EQ(f::Yields() - yields, 3U);
  static CountingThread stopped{};
  KB_REQUIRE(stopped.Start(r::ThreadConfig{}).IsOk());
  stopped.RequestStop();
  KB_CHECK(stopped.StopRequested());
  f::RunTask(3U);
  KB_CHECK_EQ(stopped.steps_done.load(), 0U);
  KB_CHECK(stopped.State() == r::ThreadState::Stopped);
  static CountingThread self_stop{};
  self_stop.stop_after = 2U;
  r::ThreadConfig periodic{};
  periodic.period = Milliseconds{5U};
  KB_REQUIRE(self_stop.Start(periodic).IsOk());
  f::RunTask(4U);
  KB_CHECK_EQ(self_stop.steps_done.load(), 2U);
  KB_CHECK(failing.Join(Milliseconds{0U}));
  static CountingThread idle{};
  KB_CHECK(!idle.Join(Milliseconds{2U}));
}

KB_TEST("freertos seam: thread creation refuses a missing body, stack or a stack below one word") {
  f::Reset();
  kb::rtos::detail::ThreadStorage storage{};
  kb::rtos::detail::ThreadCore no_run{};
  static std::array<std::byte, 64> stack{};
  kb::rtos::detail::ThreadParams p{};
  p.core = &no_run;
  p.stack = stack.data();
  p.stack_bytes = stack.size();
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, p), Status::InvalidArgument);
  kb::rtos::detail::ThreadCore runs{};
  runs.run = [](kb::rtos::detail::ThreadCore&) noexcept {};
  p.core = &runs;
  p.stack = nullptr;
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, p), Status::InvalidArgument);
  p.stack = stack.data();
  p.stack_bytes = 1U;
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, p), Status::InvalidArgument);
  p.stack_bytes = stack.size();
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, p), Status::Ok);
  kb::rtos::backend::ThreadRelease(storage);
  KB_CHECK(storage.handle == nullptr);
}

KB_TEST("freertos seam: ISR calls switch context only when a higher-priority task was woken") {
  f::Reset();
  r::Semaphore s{};
  r::Queue<uint8_t, 2> q{};
  r::EventFlags e{};
  KB_REQUIRE(s.Create(2U, 0U).IsOk() && q.Create().IsOk() && e.Create().IsOk());
  f::WakeHigherPriorityFromIsr(false);
  KB_CHECK(s.GiveFromIsr() && q.SendFromIsr(1U) && e.SetFromIsr(1U));
  KB_CHECK_EQ(f::YieldsFromIsr(), 0U);
  f::WakeHigherPriorityFromIsr(true);
  KB_CHECK(s.GiveFromIsr() && q.SendFromIsr(2U) && e.SetFromIsr(2U));
  KB_CHECK_EQ(f::YieldsFromIsr(), 3U);
  KB_CHECK(!s.GiveFromIsr() && !q.SendFromIsr(3U));
}

KB_TEST("freertos seam: critical sections nest and the clock extends past a tick wrap") {
  f::Reset();
  {
    const r::CriticalSection outer{};
    const r::CriticalSection inner{};
    KB_CHECK_EQ(f::CriticalDepth(), 2);
  }
  KB_CHECK_EQ(f::CriticalDepth(), 0);
  f::SetTick(0xFFFFFFF0UL);
  const kb::Instant before = r::Now();
  f::AdvanceTicks(0x20U);
  const kb::Instant after = r::Now();
  KB_CHECK_EQ(kb::utils::ElapsedUs(after, before), 32000U);
}

KB_TEST(
    "freertos seam: the clock in microseconds, one-tick sleeps, timer commands that never block") {
  f::Reset();
  const kb::Instant a = r::Now();
  f::AdvanceTicks(1000U);
  KB_CHECK_EQ(kb::utils::ElapsedUs(r::Now(), a), 1000000U);
  const uint32_t t0 = r::backend::TickCount();
  const uint32_t yields = f::Yields();
  r::backend::SleepTicks(1U);
  KB_CHECK_EQ(r::backend::TickCount() - t0, 1U);
  r::backend::SleepTicks(0U);
  KB_CHECK_EQ(f::Yields() - yields, 1U);
  static r::Timer t{};
  KB_REQUIRE(t.Create(&Nothing, nullptr, Milliseconds{2U}, true).IsOk());
  KB_CHECK(t.Start());
  KB_CHECK_EQ(f::LastTimerCommandTicks(), 0U);
  f::AdvanceTicks(1U);
  KB_CHECK(t.Stop());
  KB_CHECK_EQ(f::LastTimerCommandTicks(), 0U);
  r::Queue<uint32_t, 1> q{};
  KB_CHECK(q.Create().IsOk());
  static CountingThread word{};
  KB_REQUIRE(word.Start(r::ThreadConfig{}).IsOk());
  KB_CHECK_EQ(f::Task(0U).priority, 1U);
  kb::rtos::detail::ThreadStorage storage{};
  kb::rtos::detail::ThreadCore runs{};
  runs.run = [](kb::rtos::detail::ThreadCore&) noexcept {};
  static std::array<std::byte, sizeof(StackType_t)> tiny{};
  kb::rtos::detail::ThreadParams p{};
  p.core = &runs;
  p.stack = tiny.data();
  p.stack_bytes = tiny.size();
  KB_CHECK_EQ(kb::rtos::backend::ThreadCreate(storage, p), Status::Ok);
}

KB_TEST("freertos seam: a spinning thread's steps take no time") {
  f::Reset();
  static CountingThread spin{};
  spin.limit = 2U;
  KB_REQUIRE(spin.Start(r::ThreadConfig{}).IsOk());
  f::RunTask(0U);
  KB_CHECK_EQ(spin.Stats().max_step_us, 0U);
}

KB_TEST("freertos seam: the stack high-water mark in bytes, timer periods in ticks, recursive "
        "creation") {
  f::Reset();
  kb::rtos::detail::ThreadStorage storage{};
  std::size_t free_bytes = 1U;
  KB_CHECK_EQ(kb::rtos::backend::ThreadStackFree(storage, free_bytes), Status::NotReady);
  KB_CHECK_EQ(free_bytes, 0U);
  static CountingThread t{};
  KB_CHECK_EQ(t.StackFreeBytes().Error(), Status::NotReady);
  KB_REQUIRE(t.Start(r::ThreadConfig{}).IsOk());
  f::SetStackHighWater(10U);
  KB_CHECK_EQ(t.StackFreeBytes().ValueOr(0U), 10U * sizeof(StackType_t));
  static r::Timer timer{};
  KB_REQUIRE(timer.Create(&Nothing, nullptr, Milliseconds{7U}, true).IsOk());
  KB_CHECK_EQ(f::TimerPeriod(0U), 7U);
  kb::rtos::detail::TimerStorage zero{};
  kb::rtos::detail::TimerParams p{};
  p.callback = &Nothing;
  p.period_ms = 0U;
  KB_REQUIRE(kb::rtos::backend::TimerCreate(zero, p) == Status::Ok);
  KB_CHECK_EQ(f::TimerPeriod(1U), 1U); // at least one tick
  r::RecursiveMutex m{};
  f::FailNextCreate();
  KB_CHECK_EQ(m.Create().Error(), Status::Fault);
  KB_CHECK(m.Create().IsOk());
}
