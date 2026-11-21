// The host backend's deletes (ADR-0017) when the contract is broken: an object that another thread
// still holds or waits on is left in place, never destroyed under that thread, and a timer deleted
// from its own callback is left running. Host backend only. SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kb/rtos/rtos.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace r = kb::rtos;
namespace b = kb::rtos::backend;

namespace {

// Runs `body()` once on its own thread.
template <typename Body>
class Once : public r::Thread<Once<Body>, 4096U> {
public:
  explicit Once(Body body) noexcept : body_(body) {}
  static bool Setup() noexcept {
    return true;
  }
  bool Step() noexcept {
    body_();
    return false;
  }
  static void Cleanup() noexcept {}

private:
  Body body_;
};

constexpr Milliseconds kLong{5000U};

} // namespace

KB_TEST("host lifetime: a mutex another thread holds is not destroyed under it") {
  static r::detail::MutexStorage storage{};
  KB_REQUIRE(b::MutexCreate(storage) == Status::Ok);
  static const r::detail::MutexStorage kHolderView = storage; // the holder's handle to it
  static std::atomic<bool> held{false};
  static std::atomic<bool> release{false};
  auto hold = []() noexcept {
    held.store(b::MutexLock(kHolderView, 1000U));
    while (!release.load()) {
      r::SleepFor(Milliseconds{1U});
    }
    b::MutexUnlock(kHolderView);
  };
  Once<decltype(hold)> holder{hold};
  KB_REQUIRE(holder.Start(r::ThreadConfig{}).IsOk());
  for (uint32_t i = 0U; (!held.load()) && (i < 5000U); ++i) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_REQUIRE(held.load());
  b::MutexDelete(storage); // refused: the mutex stays for its holder
  KB_CHECK(storage.handle == nullptr);
  release.store(true);
  KB_CHECK(holder.Join(kLong));
  KB_CHECK(b::MutexLock(kHolderView, 0U)); // still a working mutex
  b::MutexUnlock(kHolderView);
  r::detail::MutexStorage recursive{};
  KB_REQUIRE(b::RecursiveMutexCreate(recursive) == Status::Ok);
  b::RecursiveMutexDelete(recursive);
  KB_CHECK(recursive.handle == nullptr);
  b::RecursiveMutexDelete(recursive); // a second delete has nothing to do
  b::MutexDelete(storage);
}

KB_TEST("host lifetime: a group, a semaphore or a queue with a waiter is not destroyed under it") {
  static r::detail::EventFlagsStorage flags{};
  static r::detail::SemaphoreStorage semaphore{};
  static r::detail::QueueStorage queue{};
  alignas(r::detail::kStorageAlign) static std::array<std::byte, 16> buffer{};
  KB_REQUIRE(b::EventFlagsCreate(flags) == Status::Ok);
  KB_REQUIRE(b::SemaphoreCreate(semaphore, 1U, 0U) == Status::Ok);
  KB_REQUIRE(b::QueueCreate(queue, buffer.data(), 4U, 4U) == Status::Ok);
  static const r::detail::EventFlagsStorage kFlagsView = flags;
  static const r::detail::SemaphoreStorage kSemaphoreView = semaphore;
  static const r::detail::QueueStorage kQueueView = queue;
  static std::atomic<uint32_t> woken{0U};
  auto wait = []() noexcept {
    const uint32_t seen = b::EventFlagsWait(kFlagsView, 0x1U, false, true, 5000U);
    const bool took = b::SemaphoreTake(kSemaphoreView, 5000U);
    uint32_t item = 0U;
    const bool got = b::QueueReceive(kQueueView, &item, 5000U);
    woken.store((((seen & 0x1U) != 0U) && took && got && (item == 7U)) ? 1U : 2U);
  };
  Once<decltype(wait)> waiter{wait};
  KB_REQUIRE(waiter.Start(r::ThreadConfig{}).IsOk());
  r::SleepFor(Milliseconds{50U}); // the waiter is in the group's wait
  b::EventFlagsDelete(flags);     // refused while it waits
  KB_CHECK(flags.handle == nullptr);
  b::EventFlagsSet(kFlagsView, 0x1U);
  r::SleepFor(Milliseconds{50U}); // now in the semaphore's take
  b::SemaphoreDelete(semaphore);
  KB_CHECK(semaphore.handle == nullptr);
  KB_CHECK(b::SemaphoreGive(kSemaphoreView));
  r::SleepFor(Milliseconds{50U}); // now in the queue's receive
  b::QueueDelete(queue);
  KB_CHECK(queue.handle == nullptr);
  const uint32_t item = 7U;
  KB_CHECK(b::QueueSend(kQueueView, &item, 0U));
  KB_CHECK(waiter.Join(kLong));
  KB_CHECK_EQ(woken.load(), 1U);
  b::QueueDelete(queue); // nothing left to delete
  b::SemaphoreDelete(semaphore);
  b::EventFlagsDelete(flags);
}

KB_TEST("host lifetime: a timer deleted from its own callback is left in place") {
  static r::detail::TimerStorage storage{};
  static std::atomic<uint32_t> fired{0U};
  r::detail::TimerParams p{};
  p.callback = [](void* context) noexcept {
    b::TimerDelete(*static_cast<r::detail::TimerStorage*>(context)); // cannot join its own thread
    fired.fetch_add(1U);
  };
  p.context = &storage;
  p.period_ms = 1U;
  p.periodic = false;
  KB_REQUIRE(b::TimerCreate(storage, p) == Status::Ok);
  static const r::detail::TimerStorage kView = storage;
  KB_CHECK(b::TimerStart(kView));
  for (uint32_t i = 0U; (fired.load() == 0U) && (i < 5000U); ++i) {
    r::SleepFor(Milliseconds{1U});
  }
  KB_CHECK_EQ(fired.load(), 1U);
  KB_CHECK(storage.handle == nullptr);
  KB_CHECK(b::TimerStop(kView)); // the timer object is still there
}
