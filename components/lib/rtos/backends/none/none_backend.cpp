/**
 * @file none_backend.cpp
 * @brief The `none` RTOS backend: kb::rtos without a scheduler, deterministic, for host tests and
 * bare-metal bring-up. Everything runs in one context; time is a fake tick counter (control.hpp).
 *
 * A wait that cannot succeed in one context (a held mutex, an empty queue) advances the fake clock
 * by its timeout and fails, as a real wait would after the timeout. A wait without limit
 * (UINT32_MAX ticks) that cannot succeed fails at once without advancing: in one context it could
 * never end.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/none/control.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {
namespace {

constexpr uint32_t kForever = std::numeric_limits<uint32_t>::max();
constexpr std::size_t kMaxTimers = 16U;

struct NoneMutex {
  bool locked{false};
};

struct NoneSemaphore {
  uint32_t count{0U};
  uint32_t max{1U};
};
struct NoneQueue {
  std::byte* buffer{nullptr};
  std::size_t item_bytes{0U};
  std::size_t capacity{0U};
  std::size_t head{0U};
  std::size_t count{0U};
};
struct NoneFlags {
  uint32_t bits{0U};
};
struct NoneTimer {
  detail::TimerParams params{};
  uint32_t remaining{0U};
  bool active{false};
};

struct Clock {
  uint64_t ticks{0U};
  uint32_t critical_depth{0U};
  std::array<NoneTimer*, kMaxTimers> timers{};
  std::size_t timer_count{0U};
};

Clock& State() noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-002)
  static Clock clock{};
  return clock;
}

template <typename T, std::size_t Bytes>
T& As(const detail::OpaqueStorage<Bytes>& storage) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return *static_cast<T*>(storage.handle);
}

template <typename T, std::size_t Bytes>
T* Construct(detail::OpaqueStorage<Bytes>& storage) noexcept {
  static_assert(sizeof(T) <= Bytes, "the native object must fit its opaque storage");
  static_assert(alignof(T) <= detail::kStorageAlign,
                "the native object must fit the storage alignment");
  // Placement new into the object's own storage: nothing is allocated and nothing is ever freed.
  // NOLINTBEGIN(cppcoreguidelines-owning-memory)
  void* const raw = static_cast<void*>(storage.bytes.data());
  // codeql::misra_deviation_next_line(DEV-KBC-003)
  T* const object = new (raw) T();
  // NOLINTEND(cppcoreguidelines-owning-memory)
  storage.handle = object;
  return object;
}

void FireTimers() noexcept {
  Clock& c = State();
  for (std::size_t i = 0U; i < c.timer_count; ++i) {
    NoneTimer& t = *c.timers[i];
    if (t.active) {
      --t.remaining;
      if (t.remaining == 0U) {
        t.remaining = t.params.period_ms;
        t.active = t.params.periodic;
        t.params.callback(t.params.context);
      }
    }
  }
}

// Advances by `ticks`, unless the wait has no limit (it could never end in one context).
void WaitOut(uint32_t ticks) noexcept {
  if (ticks != kForever) {
    none::AdvanceTicks(ticks);
  }
}

void CopyBytes(std::byte* to, const std::byte* from, std::size_t n) noexcept {
  for (std::size_t i = 0U; i < n; ++i) {
    to[i] = from[i];
  }
}

} // namespace

namespace none {

void AdvanceTicks(uint32_t ticks) noexcept {
  for (uint32_t i = 0U; i < ticks; ++i) {
    ++State().ticks;
    FireTimers();
  }
}

void SetTicks(uint64_t ticks) noexcept {
  State().ticks = ticks;
}

uint32_t CriticalDepth() noexcept {
  return State().critical_depth;
}

} // namespace none

namespace backend {

uint32_t TickRateHz() noexcept {
  return none::kTickRateHz;
}

uint32_t TickCount() noexcept {
  return static_cast<uint32_t>(State().ticks & 0xFFFFFFFFULL);
}

kb::Instant Now() noexcept {
  constexpr uint64_t kUsPerTick = 1000U;
  return kb::Instant{State().ticks * kUsPerTick};
}

void SleepTicks(uint32_t ticks) noexcept {
  WaitOut(ticks);
}

bool SleepUntilTick(uint32_t& previous_wake, uint32_t period_ticks) noexcept {
  const uint32_t now = TickCount();
  const uint32_t elapsed = kb::utils::ElapsedSince(now, previous_wake);
  const bool on_time = elapsed < period_ticks;
  if (on_time) {
    none::AdvanceTicks(period_ticks - elapsed);
  }
  previous_wake += period_ticks;
  return on_time;
}

void Yield() noexcept {}

void EnterCritical() noexcept {
  ++State().critical_depth;
}

void ExitCritical() noexcept {
  Clock& c = State();
  if (c.critical_depth > 0U) {
    --c.critical_depth;
  }
}

kb::Status MutexCreate(detail::MutexStorage& storage) noexcept {
  static_cast<void>(Construct<NoneMutex>(storage));
  return kb::Status::Ok;
}

bool MutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  auto& m = As<NoneMutex>(storage);
  const bool free = !m.locked;
  if (free) {
    m.locked = true;
  } else {
    WaitOut(timeout_ticks);
  }
  return free;
}

void MutexUnlock(const detail::MutexStorage& storage) noexcept {
  As<NoneMutex>(storage).locked = false;
}

void MutexDelete(detail::MutexStorage& storage) noexcept {
  storage.handle = nullptr; // a plain flag in the storage: nothing to end
}

kb::Status RecursiveMutexCreate(detail::MutexStorage& storage) noexcept {
  static_cast<void>(Construct<NoneMutex>(storage));
  return kb::Status::Ok;
}

// The only context is always the holder of a recursive mutex: every take succeeds, and the depth it
// would count is never observable.
bool RecursiveMutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  static_cast<void>(storage);
  static_cast<void>(timeout_ticks);
  return true;
}

void RecursiveMutexUnlock(const detail::MutexStorage& storage) noexcept {
  static_cast<void>(storage);
}

void RecursiveMutexDelete(detail::MutexStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status SemaphoreCreate(detail::SemaphoreStorage& storage, uint32_t max_count,
                           uint32_t initial) noexcept {
  auto* s = Construct<NoneSemaphore>(storage);
  s->max = max_count;
  s->count = initial;
  return kb::Status::Ok;
}

bool SemaphoreTake(const detail::SemaphoreStorage& storage, uint32_t timeout_ticks) noexcept {
  auto& s = As<NoneSemaphore>(storage);
  const bool available = s.count > 0U;
  if (available) {
    --s.count;
  } else {
    WaitOut(timeout_ticks);
  }
  return available;
}

bool SemaphoreGive(const detail::SemaphoreStorage& storage) noexcept {
  auto& s = As<NoneSemaphore>(storage);
  const bool room = s.count < s.max;
  if (room) {
    ++s.count;
  }
  return room;
}

bool SemaphoreGiveFromIsr(const detail::SemaphoreStorage& storage) noexcept {
  return SemaphoreGive(storage);
}

uint32_t SemaphoreCount(const detail::SemaphoreStorage& storage) noexcept {
  return As<NoneSemaphore>(storage).count;
}

void SemaphoreDelete(detail::SemaphoreStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status QueueCreate(detail::QueueStorage& storage, std::byte* buffer, std::size_t item_bytes,
                       std::size_t capacity) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((buffer != nullptr) && (item_bytes > 0U) && (capacity > 0U)) {
    auto* q = Construct<NoneQueue>(storage);
    q->buffer = buffer;
    q->item_bytes = item_bytes;
    q->capacity = capacity;
    status = kb::Status::Ok;
  }
  return status;
}

bool QueueSend(const detail::QueueStorage& storage, const void* item,
               uint32_t timeout_ticks) noexcept {
  auto& q = As<NoneQueue>(storage);
  const bool room = q.count < q.capacity;
  if (room) {
    const std::size_t slot = (q.head + q.count) % q.capacity;
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    CopyBytes(&q.buffer[slot * q.item_bytes], static_cast<const std::byte*>(item), q.item_bytes);
    ++q.count;
  } else {
    WaitOut(timeout_ticks);
  }
  return room;
}

bool QueueSendFromIsr(const detail::QueueStorage& storage, const void* item) noexcept {
  return QueueSend(storage, item, 0U);
}

bool QueueReceive(const detail::QueueStorage& storage, void* item,
                  uint32_t timeout_ticks) noexcept {
  auto& q = As<NoneQueue>(storage);
  const bool any = q.count > 0U;
  if (any) {
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    CopyBytes(static_cast<std::byte*>(item), &q.buffer[q.head * q.item_bytes], q.item_bytes);
    q.head = (q.head + 1U) % q.capacity;
    --q.count;
  } else {
    WaitOut(timeout_ticks);
  }
  return any;
}

std::size_t QueueSize(const detail::QueueStorage& storage) noexcept {
  return As<NoneQueue>(storage).count;
}

void QueueDelete(detail::QueueStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status EventFlagsCreate(detail::EventFlagsStorage& storage) noexcept {
  static_cast<void>(Construct<NoneFlags>(storage));
  return kb::Status::Ok;
}

void EventFlagsSet(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  As<NoneFlags>(storage).bits |= bits;
}

bool EventFlagsSetFromIsr(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  EventFlagsSet(storage, bits);
  return true;
}

void EventFlagsClear(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  As<NoneFlags>(storage).bits &= ~bits;
}

uint32_t EventFlagsGet(const detail::EventFlagsStorage& storage) noexcept {
  return As<NoneFlags>(storage).bits;
}

uint32_t EventFlagsWait(const detail::EventFlagsStorage& storage, uint32_t bits, bool wait_all,
                        bool clear_on_exit, uint32_t timeout_ticks) noexcept {
  auto& f = As<NoneFlags>(storage);
  const uint32_t seen = f.bits;
  const bool met = wait_all ? ((seen & bits) == bits) : ((seen & bits) != 0U);
  if (met) {
    if (clear_on_exit) {
      f.bits &= ~bits;
    }
  } else {
    WaitOut(timeout_ticks);
  }
  return met ? seen : f.bits;
}

void EventFlagsDelete(detail::EventFlagsStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status TimerCreate(detail::TimerStorage& storage, const detail::TimerParams& params) noexcept {
  Clock& c = State();
  kb::Status status = kb::Status::Full;
  if (c.timer_count < kMaxTimers) {
    auto* t = Construct<NoneTimer>(storage);
    t->params = params;
    c.timers[c.timer_count] = t;
    ++c.timer_count;
    status = kb::Status::Ok;
  }
  return status;
}

bool TimerStart(const detail::TimerStorage& storage) noexcept {
  auto& t = As<NoneTimer>(storage);
  t.remaining = t.params.period_ms;
  t.active = true;
  return true;
}

bool TimerStop(const detail::TimerStorage& storage) noexcept {
  As<NoneTimer>(storage).active = false;
  return true;
}

void TimerDelete(detail::TimerStorage& storage) noexcept {
  // Out of the table, so the fake clock never runs the callback of a timer that has ended.
  Clock& c = State();
  const NoneTimer* const gone = &As<NoneTimer>(storage);
  std::size_t kept = 0U;
  for (std::size_t i = 0U; i < c.timer_count; ++i) {
    if (c.timers[i] != gone) {
      c.timers[kept] = c.timers[i];
      ++kept;
    }
  }
  c.timer_count = kept; // entries past the count are never read (a create overwrites them)
  storage.handle = nullptr;
}

kb::Status ThreadCreate(detail::ThreadStorage& storage,
                        const detail::ThreadParams& params) noexcept {
  storage.handle = nullptr; // no scheduler: nothing to start
  static_cast<void>(params);
  return kb::Status::Unsupported;
}

void ThreadExit(const detail::ThreadStorage& storage) noexcept {
  static_cast<void>(storage);
}

void ThreadRelease(detail::ThreadStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status ThreadStackFree(const detail::ThreadStorage& storage, std::size_t& free_bytes) noexcept {
  static_cast<void>(storage);
  free_bytes = 0U;
  return kb::Status::Unsupported; // no thread to measure
}

} // namespace backend
} // namespace kb::rtos
