/**
 * @file host_backend.cpp
 * @brief The `host` RTOS backend: kb::rtos over the C++ standard thread library, for PC tests. Test
 * code, outside the MISRA claim (components/ports/host/README.md).
 *
 * Every object is a standard-library object constructed in the kb object's opaque storage and
 * destroyed by the kb object's destructor (the *Delete functions, ADR-0017), so no mutex or
 * condition variable is ever constructed over one that was not destroyed. An object that a thread
 * still holds or waits on when its kb object ends is left in place instead (never destroyed under
 * that thread). Waits use steady_clock deadlines; the tick is one millisecond of that clock. Timers
 * run on one thread each.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/backend_rules.hpp"

namespace kb::rtos {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kForever = std::numeric_limits<uint32_t>::max();

Clock::time_point Epoch() noexcept {
  static const Clock::time_point kEpoch = Clock::now();
  return kEpoch;
}

std::chrono::milliseconds Ticks(uint32_t ticks) noexcept {
  return std::chrono::milliseconds{ticks};
}

uint32_t& CriticalDepth() noexcept {
  thread_local uint32_t depth = 0U;
  return depth;
}
std::recursive_mutex& CriticalMutex() noexcept {
  static std::recursive_mutex m;
  return m;
}

struct HostMutex {
  std::timed_mutex m;
};
struct HostRecursiveMutex {
  std::recursive_timed_mutex m;
};
struct HostSemaphore {
  std::mutex m;
  std::condition_variable cv;
  uint32_t count{0U};
  uint32_t max{1U};
  uint32_t waiters{0U}; ///< Threads in a take (under `m`).
};
struct HostQueue {
  std::mutex m;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  std::byte* buffer{nullptr};
  std::size_t item_bytes{0U};
  std::size_t capacity{0U};
  std::size_t head{0U};
  std::size_t count{0U};
  uint32_t waiters{0U}; ///< Threads in a send or a receive (under `m`).
};
struct HostFlags {
  std::mutex m;
  std::condition_variable cv;
  uint32_t bits{0U};
  uint32_t waiters{0U}; ///< Threads in a wait (under `m`).
};
struct HostTimer {
  std::mutex m;
  std::condition_variable cv;
  detail::TimerParams params{};
  std::thread worker;
  uint64_t generation{0U};
  bool active{false};
  bool quit{false};
  ~HostTimer() {
    {
      const std::lock_guard<std::mutex> lock{m};
      quit = true;
    }
    cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }
};
struct HostThread {
  std::thread t;
};

template <typename T, std::size_t Bytes>
T& As(const detail::OpaqueStorage<Bytes>& storage) noexcept {
  return *static_cast<T*>(storage.handle);
}

template <typename T, std::size_t Bytes>
T* Construct(detail::OpaqueStorage<Bytes>& storage) noexcept {
  static_assert(sizeof(T) <= Bytes, "the native object must fit its opaque storage");
  static_assert(alignof(T) <= detail::kStorageAlign,
                "the native object must fit the storage alignment");
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  T* object = new (static_cast<void*>(storage.bytes.data())) T{};
  storage.handle = object;
  return object;
}

// Waits on `cv` until `ready()` or the timeout; kForever waits without limit. `waiters` counts the
// thread in the wait (the caller holds `lock`), so a delete never destroys `cv` under it.
template <typename Pred>
bool WaitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, uint32_t& waiters,
             uint32_t ticks, Pred ready) {
  bool ok = false;
  ++waiters;
  if (ticks == kForever) {
    cv.wait(lock, ready);
    ok = true;
  } else {
    ok = cv.wait_for(lock, Ticks(ticks), ready);
  }
  --waiters;
  return ok;
}

// True when no thread holds `object.m` or waits on the object, so it may be destroyed.
template <typename T>
bool Unused(T& object) noexcept {
  const std::unique_lock<std::mutex> lock{object.m, std::try_to_lock};
  return lock.owns_lock() && (object.waiters == 0U);
}

// Ends the native object of `storage` (constructed by Construct) when `unused`, and drops the
// handle either way: a used object is left in place, never destroyed under a thread.
template <typename T, std::size_t Bytes>
void Destroy(detail::OpaqueStorage<Bytes>& storage, bool unused) noexcept {
  if (unused) {
    As<T>(storage).~T();
  }
  storage.handle = nullptr;
}

void TimerLoop(HostTimer* t) {
  std::unique_lock<std::mutex> lock{t->m};
  while (!t->quit) {
    if (!t->active) {
      t->cv.wait(lock);
      continue;
    }
    const uint64_t generation = t->generation;
    const auto due = Clock::now() + Ticks(t->params.period_ms);
    const bool changed = t->cv.wait_until(lock, due, [t, generation] {
      return t->quit || (!t->active) || (t->generation != generation);
    });
    if (!changed) {
      t->active = t->params.periodic;
      const detail::EntryFn fn = t->params.callback;
      void* const ctx = t->params.context;
      lock.unlock();
      fn(ctx);
      lock.lock();
    }
  }
}

} // namespace

namespace backend {

uint32_t TickRateHz() noexcept {
  return 1000U;
}

uint32_t TickCount() noexcept {
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Epoch()).count();
  return static_cast<uint32_t>(static_cast<uint64_t>(ms) & 0xFFFFFFFFULL);
}

kb::Instant Now() noexcept {
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - Epoch()).count();
  return kb::Instant{static_cast<uint64_t>(us)};
}

void SleepTicks(uint32_t ticks) noexcept {
  if (ticks == 0U) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(Ticks(ticks));
  }
}

bool SleepUntilTick(uint32_t& previous_wake, uint32_t period_ticks) noexcept {
  const detail::DelayUntilPlan plan =
      detail::PlanDelayUntil(TickCount(), previous_wake, period_ticks);
  if (plan.on_time) {
    std::this_thread::sleep_for(Ticks(plan.sleep_ticks));
  }
  previous_wake += period_ticks;
  return plan.on_time;
}

void Yield() noexcept {
  std::this_thread::yield();
}

void EnterCritical() noexcept {
  CriticalMutex().lock();
  ++CriticalDepth();
}

void ExitCritical() noexcept {
  if (CriticalDepth() > 0U) {
    --CriticalDepth();
    CriticalMutex().unlock();
  }
}

kb::Status MutexCreate(detail::MutexStorage& storage) noexcept {
  static_cast<void>(Construct<HostMutex>(storage));
  return kb::Status::Ok;
}

bool MutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  auto& m = As<HostMutex>(storage);
  bool ok = false;
  if (timeout_ticks == kForever) {
    m.m.lock();
    ok = true;
  } else if (timeout_ticks == 0U) {
    ok = m.m.try_lock();
  } else {
    ok = m.m.try_lock_for(Ticks(timeout_ticks));
  }
  return ok;
}

void MutexUnlock(const detail::MutexStorage& storage) noexcept {
  As<HostMutex>(storage).m.unlock();
}

void MutexDelete(detail::MutexStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    auto& m = As<HostMutex>(storage);
    const bool unused = m.m.try_lock();
    if (unused) {
      m.m.unlock();
    }
    Destroy<HostMutex>(storage, unused);
  }
}

kb::Status RecursiveMutexCreate(detail::MutexStorage& storage) noexcept {
  static_cast<void>(Construct<HostRecursiveMutex>(storage));
  return kb::Status::Ok;
}

bool RecursiveMutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  auto& m = As<HostRecursiveMutex>(storage);
  bool ok = false;
  if (timeout_ticks == kForever) {
    m.m.lock();
    ok = true;
  } else if (timeout_ticks == 0U) {
    ok = m.m.try_lock();
  } else {
    ok = m.m.try_lock_for(Ticks(timeout_ticks));
  }
  return ok;
}

void RecursiveMutexUnlock(const detail::MutexStorage& storage) noexcept {
  As<HostRecursiveMutex>(storage).m.unlock();
}

void RecursiveMutexDelete(detail::MutexStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    auto& m = As<HostRecursiveMutex>(storage);
    const bool unused = m.m.try_lock(); // another thread's hold refuses it (the caller's cannot)
    if (unused) {
      m.m.unlock();
    }
    Destroy<HostRecursiveMutex>(storage, unused);
  }
}

kb::Status SemaphoreCreate(detail::SemaphoreStorage& storage, uint32_t max_count,
                           uint32_t initial) noexcept {
  auto* s = Construct<HostSemaphore>(storage);
  s->max = max_count;
  s->count = initial;
  return kb::Status::Ok;
}

bool SemaphoreTake(const detail::SemaphoreStorage& storage, uint32_t timeout_ticks) noexcept {
  auto& s = As<HostSemaphore>(storage);
  std::unique_lock<std::mutex> lock{s.m};
  const bool ok = WaitFor(s.cv, lock, s.waiters, timeout_ticks, [&s] { return s.count > 0U; });
  if (ok) {
    --s.count;
  }
  return ok;
}

bool SemaphoreGive(const detail::SemaphoreStorage& storage) noexcept {
  auto& s = As<HostSemaphore>(storage);
  bool room = false;
  {
    const std::lock_guard<std::mutex> lock{s.m};
    room = s.count < s.max;
    if (room) {
      ++s.count;
    }
  }
  s.cv.notify_one();
  return room;
}

bool SemaphoreGiveFromIsr(const detail::SemaphoreStorage& storage) noexcept {
  return SemaphoreGive(storage);
}

uint32_t SemaphoreCount(const detail::SemaphoreStorage& storage) noexcept {
  auto& s = As<HostSemaphore>(storage);
  const std::lock_guard<std::mutex> lock{s.m};
  return s.count;
}

void SemaphoreDelete(detail::SemaphoreStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    Destroy<HostSemaphore>(storage, Unused(As<HostSemaphore>(storage)));
  }
}

kb::Status QueueCreate(detail::QueueStorage& storage, std::byte* buffer, std::size_t item_bytes,
                       std::size_t capacity) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((buffer != nullptr) && (item_bytes > 0U) && (capacity > 0U)) {
    auto* q = Construct<HostQueue>(storage);
    q->buffer = buffer;
    q->item_bytes = item_bytes;
    q->capacity = capacity;
    status = kb::Status::Ok;
  }
  return status;
}

bool QueueSend(const detail::QueueStorage& storage, const void* item,
               uint32_t timeout_ticks) noexcept {
  auto& q = As<HostQueue>(storage);
  bool ok = false;
  {
    std::unique_lock<std::mutex> lock{q.m};
    ok = WaitFor(q.not_full, lock, q.waiters, timeout_ticks, [&q] { return q.count < q.capacity; });
    if (ok) {
      const std::size_t slot = (q.head + q.count) % q.capacity;
      std::memcpy(&q.buffer[slot * q.item_bytes], item, q.item_bytes);
      ++q.count;
    }
  }
  if (ok) {
    q.not_empty.notify_one();
  }
  return ok;
}

bool QueueSendFromIsr(const detail::QueueStorage& storage, const void* item) noexcept {
  return QueueSend(storage, item, 0U);
}

bool QueueReceive(const detail::QueueStorage& storage, void* item,
                  uint32_t timeout_ticks) noexcept {
  auto& q = As<HostQueue>(storage);
  bool ok = false;
  {
    std::unique_lock<std::mutex> lock{q.m};
    ok = WaitFor(q.not_empty, lock, q.waiters, timeout_ticks, [&q] { return q.count > 0U; });
    if (ok) {
      std::memcpy(item, &q.buffer[q.head * q.item_bytes], q.item_bytes);
      q.head = (q.head + 1U) % q.capacity;
      --q.count;
    }
  }
  if (ok) {
    q.not_full.notify_one();
  }
  return ok;
}

std::size_t QueueSize(const detail::QueueStorage& storage) noexcept {
  auto& q = As<HostQueue>(storage);
  const std::lock_guard<std::mutex> lock{q.m};
  return q.count;
}

void QueueDelete(detail::QueueStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    Destroy<HostQueue>(storage, Unused(As<HostQueue>(storage)));
  }
}

kb::Status EventFlagsCreate(detail::EventFlagsStorage& storage) noexcept {
  static_cast<void>(Construct<HostFlags>(storage));
  return kb::Status::Ok;
}

void EventFlagsSet(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  auto& f = As<HostFlags>(storage);
  {
    const std::lock_guard<std::mutex> lock{f.m};
    f.bits |= bits;
  }
  f.cv.notify_all();
}

bool EventFlagsSetFromIsr(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  EventFlagsSet(storage, bits);
  return true;
}

void EventFlagsClear(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  auto& f = As<HostFlags>(storage);
  const std::lock_guard<std::mutex> lock{f.m};
  f.bits &= ~bits;
}

uint32_t EventFlagsGet(const detail::EventFlagsStorage& storage) noexcept {
  auto& f = As<HostFlags>(storage);
  const std::lock_guard<std::mutex> lock{f.m};
  return f.bits;
}

uint32_t EventFlagsWait(const detail::EventFlagsStorage& storage, uint32_t bits, bool wait_all,
                        bool clear_on_exit, uint32_t timeout_ticks) noexcept {
  auto& f = As<HostFlags>(storage);
  std::unique_lock<std::mutex> lock{f.m};
  const auto met = [&f, bits, wait_all] {
    return wait_all ? ((f.bits & bits) == bits) : ((f.bits & bits) != 0U);
  };
  const bool ok = WaitFor(f.cv, lock, f.waiters, timeout_ticks, met);
  const uint32_t seen = f.bits;
  if (ok && clear_on_exit) {
    f.bits &= ~bits;
  }
  return seen;
}

void EventFlagsDelete(detail::EventFlagsStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    Destroy<HostFlags>(storage, Unused(As<HostFlags>(storage)));
  }
}

kb::Status TimerCreate(detail::TimerStorage& storage, const detail::TimerParams& params) noexcept {
  auto* t = Construct<HostTimer>(storage);
  t->params = params;
  t->worker = std::thread{TimerLoop, t};
  return kb::Status::Ok;
}

bool TimerStart(const detail::TimerStorage& storage) noexcept {
  auto& t = As<HostTimer>(storage);
  {
    const std::lock_guard<std::mutex> lock{t.m};
    t.active = true;
    ++t.generation;
  }
  t.cv.notify_all();
  return true;
}

bool TimerStop(const detail::TimerStorage& storage) noexcept {
  auto& t = As<HostTimer>(storage);
  {
    const std::lock_guard<std::mutex> lock{t.m};
    t.active = false;
    ++t.generation;
  }
  t.cv.notify_all();
  return true;
}

void TimerDelete(detail::TimerStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    // ~HostTimer ends and joins the timer's thread (after a running callback returns); a delete
    // from the timer's own callback cannot join that thread, so the timer is left in place.
    const bool own_callback = As<HostTimer>(storage).worker.get_id() == std::this_thread::get_id();
    Destroy<HostTimer>(storage, !own_callback);
  }
}

kb::Status ThreadCreate(detail::ThreadStorage& storage,
                        const detail::ThreadParams& params) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((params.core != nullptr) && (params.core->run != nullptr) && (params.stack != nullptr)) {
    auto* t = Construct<HostThread>(storage);
    // The static stack is not used on the host: the standard library gives the thread its own.
    detail::ThreadCore* const core = params.core;
    t->t = std::thread{[core] { core->run(*core); }};
    status = kb::Status::Ok;
  }
  return status;
}

void ThreadExit(const detail::ThreadStorage& storage) noexcept {
  static_cast<void>(storage);
}

void ThreadRelease(detail::ThreadStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    auto& t = As<HostThread>(storage);
    if (t.t.joinable()) {
      t.t.join();
    }
    t.~HostThread();
    storage.handle = nullptr;
  }
}

kb::Status ThreadStackFree(const detail::ThreadStorage& storage, std::size_t& free_bytes) noexcept {
  static_cast<void>(storage);
  free_bytes = 0U;
  return kb::Status::Unsupported; // the standard library owns a host thread's stack
}

} // namespace backend
} // namespace kb::rtos
