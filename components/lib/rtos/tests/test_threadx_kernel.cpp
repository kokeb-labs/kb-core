// kb::rtos on the real ThreadX kernel (Linux port, KB_THREADX_HOST_TESTS) beyond the threaded suite
// of test_rtos_host.cpp: priority inheritance resolves an inversion, priorities preempt as mapped,
// and the stack high-water mark is measured. The runner thread runs at kb priority 15
// (tests/harness/kb_test_main_threadx.cpp). SPDX-License-Identifier: MIT Copyright (c) 2026 The
// kb-core authors
#include <atomic>
#include <cstdint>

#include "counting_thread.hpp"
#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;

namespace {

r::Mutex g_shared{};
std::atomic<uint64_t> g_high_got_us{0U};
std::atomic<uint64_t> g_medium_done_us{0U};
std::atomic<bool> g_low_holds{false};

void Spin(uint64_t us) noexcept {
  const kb::Instant start = r::Now();
  while (kb::utils::ElapsedUs(r::Now(), start) < us) {
  }
}

// Holds the mutex while it computes for 50 ms: it needs the CPU to give the mutex back.
class Low : public r::Thread<Low, 8192U> {
public:
  static bool Setup() noexcept {
    const bool got = g_shared.TryLockFor(Milliseconds{100U});
    g_low_holds.store(got);
    return got;
  }
  static bool Step() noexcept {
    Spin(50000U);
    g_shared.Unlock();
    return false;
  }
  static void Cleanup() noexcept {}
};

// Waits for the mutex the low thread holds.
class High : public r::Thread<High, 8192U> {
public:
  static bool Setup() noexcept {
    return true;
  }
  static bool Step() noexcept {
    if (g_shared.TryLockFor(Milliseconds{5000U})) {
      g_high_got_us.store(r::Now().Us());
      g_shared.Unlock();
    }
    return false;
  }
  static void Cleanup() noexcept {}
};

// Computes for 300 ms at a priority between the two: without inheritance it would starve the low
// thread.
class Medium : public r::Thread<Medium, 8192U> {
public:
  static bool Setup() noexcept {
    return true;
  }
  static bool Step() noexcept {
    Spin(300000U);
    g_medium_done_us.store(r::Now().Us());
    return false;
  }
  static void Cleanup() noexcept {}
};

} // namespace

KB_TEST("threadx kernel: priority inheritance lets the holder finish before a medium thread") {
  KB_REQUIRE(g_shared.Create().IsOk());
  static Low low{};
  static High high{};
  static Medium medium{};
  r::ThreadConfig cfg{};
  cfg.priority = 10U; // below the runner
  KB_REQUIRE(low.Start(cfg).IsOk());
  for (uint32_t waited = 0U; (!g_low_holds.load()) && (waited < 1000U); ++waited) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_REQUIRE(g_low_holds.load());
  cfg.priority = 25U; // above everything: blocks on the mutex at once, the low thread inherits 25
  KB_REQUIRE(high.Start(cfg).IsOk());
  cfg.priority = 20U; // above the low thread's own priority, below the inherited one
  KB_REQUIRE(medium.Start(cfg).IsOk());
  KB_CHECK(low.Join(Milliseconds{5000U}) && high.Join(Milliseconds{5000U}) &&
           medium.Join(Milliseconds{5000U}));
  KB_CHECK(g_high_got_us.load() != 0U);
  KB_CHECK(g_medium_done_us.load() != 0U);
  KB_CHECK(g_high_got_us.load() < g_medium_done_us.load());
}

KB_TEST("threadx kernel: a higher-priority thread runs as soon as it starts") {
  static CountingThread urgent{};
  urgent.limit = 1U;
  r::ThreadConfig cfg{};
  cfg.priority = 25U;
  KB_REQUIRE(urgent.Start(cfg).IsOk());
  // The new thread preempted the runner inside Start(): it has finished without the runner waiting.
  KB_CHECK_EQ(urgent.steps_done.load(), 1U);
  KB_CHECK(urgent.Join(Milliseconds{100U}));
}

KB_TEST("threadx kernel: the stack high-water mark is measured while the thread runs") {
  static CountingThread t{};
  r::ThreadConfig cfg{};
  cfg.period = Milliseconds{1U};
  KB_REQUIRE(t.StartAndWaitToVerify(cfg, Milliseconds{1000U}).IsOk());
  const kb::Result<std::size_t> free_bytes = t.StackFreeBytes();
  KB_CHECK(free_bytes.IsOk());
  // The Linux port runs each thread on its own pthread stack, so the kb stack shows only what the
  // kernel wrote into it: the measure is in range, not a real depth (that is checked on the fake
  // kernel).
  KB_CHECK(free_bytes.ValueOr(4097U) <= 4096U);
  t.RequestStop();
  KB_CHECK(t.Join(Milliseconds{1000U}));
  KB_CHECK_EQ(t.StackFreeBytes().Error(), Status::NotReady);
  KB_CHECK_EQ(r::TickRateHz(), static_cast<uint32_t>(KB_TEST_TICK_HZ));
}
