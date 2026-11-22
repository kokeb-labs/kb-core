// The none backend's own behaviour: no threads, deterministic waits without a limit, nesting
// critical sections. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "counting_thread.hpp"
#include "kb/rtos/none/control.hpp"
#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
namespace r = kb::rtos;

namespace {
void Nothing(void* context) noexcept {
  static_cast<void>(context);
}
} // namespace

KB_TEST("none: threads cannot start; the object stays idle and can be destroyed") {
  CountingThread t{};
  KB_CHECK_EQ(t.Start(r::ThreadConfig{}).Error(), kb::Status::Unsupported);
  KB_CHECK(t.State() == r::ThreadState::Idle);
  const uint32_t t0 = r::backend::TickCount();
  KB_CHECK(!t.Join(Milliseconds{2U}));
  // Polled once a tick until 2 ms had passed on the clock: exactly the timeout, not more.
  KB_CHECK_EQ(r::backend::TickCount() - t0, 2U);
}

KB_TEST("none: a wait without a limit that cannot succeed fails at once") {
  r::Mutex m{};
  KB_REQUIRE(m.Create().IsOk());
  KB_REQUIRE(m.TryLock());
  const uint32_t t0 = r::backend::TickCount();
  KB_CHECK(!m.TryLockFor(kb::kMaxMilliseconds));
  KB_CHECK_EQ(r::backend::TickCount(), t0);
  m.Unlock();
}

KB_TEST("none: critical sections count their depth and never go below zero") {
  KB_CHECK_EQ(r::none::CriticalDepth(), 0U);
  {
    const r::CriticalSection a{};
    KB_CHECK_EQ(r::none::CriticalDepth(), 1U);
  }
  r::backend::ExitCritical();
  KB_CHECK_EQ(r::none::CriticalDepth(), 0U);
}

KB_TEST("none: the 64-bit clock keeps counting past the 32-bit tick") {
  r::none::SetTicks(0xFFFFFFFFULL);
  const kb::Instant before = r::Now();
  r::none::AdvanceTicks(2U);
  KB_CHECK_EQ(r::backend::TickCount(), 1U);
  KB_CHECK_EQ(kb::utils::ElapsedUs(r::Now(), before), 2000U);
}

KB_TEST("none: timer slots are bounded") {
  std::array<r::Timer, 17> timers{}; // their ends free the slots for the later cases
  uint32_t ok = 0U;
  for (r::Timer& t : timers) {
    ok += t.Create(&Nothing, nullptr, Milliseconds{1U}, true).IsOk() ? 1U : 0U;
  }
  // test_rtos_single.cpp created two timers before this test: the backend holds exactly 16.
  KB_CHECK_EQ(ok, 14U);
}

KB_TEST("none: queue arguments are checked; thread exit and release are harmless") {
  r::detail::QueueStorage q{};
  std::array<std::byte, 8> buffer{};
  KB_CHECK_EQ(r::backend::QueueCreate(q, nullptr, 4U, 2U), kb::Status::InvalidArgument);
  KB_CHECK_EQ(r::backend::QueueCreate(q, buffer.data(), 0U, 2U), kb::Status::InvalidArgument);
  KB_CHECK_EQ(r::backend::QueueCreate(q, buffer.data(), 4U, 0U), kb::Status::InvalidArgument);
  r::detail::ThreadStorage t{};
  r::backend::ThreadExit(t);
  r::backend::ThreadRelease(t);
  KB_CHECK(t.handle == nullptr);
  std::size_t free_bytes = 1U;
  KB_CHECK_EQ(r::backend::ThreadStackFree(t, free_bytes), kb::Status::Unsupported);
  KB_CHECK_EQ(free_bytes, 0U);
}

KB_TEST("none: a deleted timer leaves the table: no callback after its end, its slot is free") {
  static uint32_t fired = 0U;
  const auto count = [](void* context) noexcept { ++*static_cast<uint32_t*>(context); };
  r::Timer kept{};
  KB_REQUIRE(kept.Create(&Nothing, nullptr, Milliseconds{1U}, true).IsOk());
  r::detail::TimerStorage storage{};
  {
    r::Timer gone{};
    KB_REQUIRE(gone.Create(count, &fired, Milliseconds{1U}, true).IsOk());
    KB_CHECK(gone.Start() && kept.Start());
    r::none::AdvanceTicks(2U);
    KB_CHECK_EQ(fired, 2U);
  }
  r::none::AdvanceTicks(3U);
  KB_CHECK_EQ(fired, 2U); // the ended timer is out of the table; the kept one still runs
  r::detail::TimerParams p{};
  p.callback = count;
  p.context = &fired;
  KB_REQUIRE(r::backend::TimerCreate(storage, p) == kb::Status::Ok); // the freed slot
  r::backend::TimerDelete(storage);
  KB_CHECK(storage.handle == nullptr);
  r::detail::MutexStorage m{};
  KB_REQUIRE(r::backend::MutexCreate(m) == kb::Status::Ok);
  r::backend::MutexDelete(m);
  KB_CHECK(m.handle == nullptr);
}

KB_TEST("none: deleting the last timer of a full table frees exactly its slot") {
  static std::array<r::detail::TimerStorage, 18> slots{};
  r::detail::TimerParams p{};
  p.callback = &Nothing;
  std::size_t made = 0U;
  while ((made < slots.size()) && (r::backend::TimerCreate(slots[made], p) == kb::Status::Ok)) {
    ++made;
  }
  KB_REQUIRE((made > 0U) && (made < slots.size())); // the table filled before the slots ran out
  r::backend::TimerDelete(slots[made - 1U]);        // the last entry of a full table
  KB_CHECK(r::backend::TimerCreate(slots[made - 1U], p) == kb::Status::Ok);
  KB_CHECK(r::backend::TimerCreate(slots[made], p) == kb::Status::Full); // no second slot came free
  for (std::size_t i = 0U; i < made; ++i) {
    r::backend::TimerDelete(slots[i]); // free them for the later cases
  }
}
