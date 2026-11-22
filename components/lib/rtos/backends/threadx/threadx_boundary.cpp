/**
 * @file threadx_boundary.cpp
 * @brief The Eclipse ThreadX backend of kb::rtos: the only kb-core file that includes ThreadX
 * headers (threadx_isr.cpp maps the interrupt-safe calls onto this file's functions). Static
 * allocation only (every `tx_*_create` on a control block and stack the kb object owns); no logic
 * beyond converting types, mapping results and the few rules ThreadX does not have (a non-recursive
 * mutex, a semaphore maximum, delay-until).
 *
 * Boundary file (deviations/boundary-files.yml). ThreadX is SOUP, reached with -isystem; the
 * conversions from void* are DEV-KBC-001, the tick extension and timer table DEV-KBC-002. Each
 * native block puts the ThreadX control block first, so the block and the control block share one
 * address.
 *
 * Configuration (components/lib/rtos/backends/threadx/README.md): timers and timeouts (no
 * TX_NO_TIMER), TX_TIMER_TICKS_PER_SECOND equal to the real tick, a 32-bit or wider ULONG; for
 * Thread::StackFreeBytes() TX_ENABLE_STACK_CHECKING.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/backend_rules.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

#include "tx_api.h"

#if defined(TX_NO_TIMER)
static_assert(sizeof(ULONG) == 0U,
              "kb::rtos needs ThreadX timers and timeouts: TX_NO_TIMER must not be defined");
#endif
// The tick must be set on purpose: tx_api.h silently defaults it to 100 when tx_user.h is not
// included or does not set it. The build passes the rate it was told (KB_THREADX_TICK_HZ).
#if !defined(KB_THREADX_TICK_HZ)
static_assert(sizeof(ULONG) == 0U, "build the ThreadX backend with KB_THREADX_TICK_HZ set");
#else
static_assert(TX_TIMER_TICKS_PER_SECOND == KB_THREADX_TICK_HZ,
              "TX_TIMER_TICKS_PER_SECOND differs from KB_THREADX_TICK_HZ: is tx_user.h included?");
#endif
// The kernel's control blocks as the product compiled them, when it says (KB_THREADX_THREAD_BYTES:
// sizeof(TX_THREAD) in the kernel's own build): options that change the layouts must agree.
#if defined(KB_THREADX_THREAD_BYTES)
static_assert(
    sizeof(TX_THREAD) == KB_THREADX_THREAD_BYTES,
    "TX_THREAD differs from the kernel's: build the backend with the kernel's TX_* options");
#endif

namespace kb::rtos {
namespace {

/// The largest message ThreadX queues move, in native words.
inline constexpr std::size_t kMaxQueueWords = 16U;
/// Timers that may exist at once: the expiry callback finds its timer by index in a table.
inline constexpr std::size_t kMaxTimers = 32U;
/// Characters of a thread's name kept for the kernel's diagnostics (the rest is cut).
inline constexpr std::size_t kNameChars = 16U;

/// A counting semaphore and the maximum count kb gives it (ThreadX semaphores have none).
struct SemaphoreBlock {
  TX_SEMAPHORE semaphore;
  ULONG max{};
};

/// A queue and the item size the kb queue carries (ThreadX moves whole words).
struct QueueBlock {
  TX_QUEUE queue;
  std::size_t item_bytes{};
};

/// A timer and the kb callback it runs.
struct TimerBlock {
  TX_TIMER timer;
  detail::TimerParams params{};
  ULONG period_ticks{};
  ULONG slot{};
};

/// A thread, the kb thread it runs and a writable copy of its name.
struct ThreadBlock {
  TX_THREAD thread;
  detail::ThreadCore* core{nullptr};
  std::array<CHAR, kNameChars> name{};
};

static_assert(sizeof(ULONG) >= sizeof(uint32_t), "ThreadX ULONG must hold 32 bits");
static_assert(
    (detail::kQueueWordBytes % sizeof(ULONG)) == 0U,
    "a kb queue slot must be whole ThreadX words: KB_THREADX_QUEUE_WORD_BYTES = sizeof(ULONG)");
static_assert(sizeof(TX_MUTEX) <= detail::kMutexBytes, "a ThreadX mutex must fit kMutexBytes");
static_assert(sizeof(SemaphoreBlock) <= detail::kSemaphoreBytes,
              "a ThreadX semaphore must fit kSemaphoreBytes");
static_assert(sizeof(QueueBlock) <= detail::kQueueBytes, "a ThreadX queue must fit kQueueBytes");
static_assert(sizeof(TX_EVENT_FLAGS_GROUP) <= detail::kEventFlagsBytes,
              "a ThreadX event-flag group must fit kEventFlagsBytes");
static_assert(sizeof(TimerBlock) <= detail::kTimerBytes, "a ThreadX timer must fit kTimerBytes");
static_assert(sizeof(ThreadBlock) <= detail::kThreadBytes,
              "a ThreadX thread must fit kThreadBytes");
static_assert(alignof(ThreadBlock) <= detail::kStorageAlign,
              "the ThreadX blocks must fit the storage alignment");

/// A kb wait in ticks as a ThreadX wait option: UINT32_MAX is TX_WAIT_FOREVER.
[[nodiscard]] ULONG WaitOption(uint32_t ticks) noexcept {
  return (ticks == std::numeric_limits<uint32_t>::max()) ? TX_WAIT_FOREVER
                                                         : static_cast<ULONG>(ticks);
}

/// The block constructed in a kb object's opaque bytes (DEV-KBC-001: from void*).
template <typename T, std::size_t Bytes>
[[nodiscard]] T* Block(const detail::OpaqueStorage<Bytes>& storage) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<T*>(storage.handle);
}

// The name every non-thread object is created with (ThreadX keeps the pointer and never writes it).
CHAR* ObjectName() noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-002)
  static std::array name{'k', 'b', '\0'};
  return name.data();
}

// Raw kb-owned bytes as the ThreadX type (DEV-KBC-001: through void*). Not yet the object's handle.
template <typename T, std::size_t Bytes>
T* Construct(detail::OpaqueStorage<Bytes>& storage) noexcept {
  void* const raw = storage.bytes.data();
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<T*>(raw);
}

// Makes a created block the object's handle only once the kernel has filled it in; a failed
// create leaves no handle.
template <typename T, std::size_t Bytes>
kb::Status Publish(detail::OpaqueStorage<Bytes>& storage, T* block, UINT result) noexcept {
  const bool created = result == TX_SUCCESS;
  storage.handle = created ? block : nullptr;
  return created ? kb::Status::Ok : kb::Status::Fault;
}

// One queue message: the item copied into whole native words, as ThreadX moves them.
using Message = std::array<ULONG, kMaxQueueWords>;
std::byte* MessageBytes(Message& message) noexcept {
  return std::as_writable_bytes(std::span<ULONG>(message)).data();
}

// The process-wide state a kernel port would hold: the critical-section nesting, the 64-bit tick
// extension and the timer table. Written only with interrupts masked; a timer slot is read by its
// expiry after it was published.
struct State {
  uint32_t critical_depth{};
  UINT critical_posture{};
  detail::TickExtender ticks{};
  std::array<TimerBlock*, kMaxTimers> timers{};
};
State& Shared() noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-002)
  static State state{};
  return state;
}

// The entry of every kb thread: the running TX_THREAD is the first member of its ThreadBlock.
void ThreadEntry(ULONG input) {
  static_cast<void>(input);
  void* const self = tx_thread_identify();
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  auto* const block = static_cast<ThreadBlock*>(self);
  block->core->run(*block->core); // ThreadCreate refused a missing core
}

// The expiry of every kb timer: `slot` indexes the timer table.
void TimerEntry(ULONG slot) {
  const TimerBlock* block = nullptr;
  if (slot < kMaxTimers) {
    block = Shared().timers[static_cast<std::size_t>(slot)];
  }
  if (block != nullptr) {
    block->params.callback(block->params.context);
  }
}

// The holder of a mutex, or null.
TX_THREAD* Owner(TX_MUTEX* mutex) noexcept {
  TX_THREAD* owner = nullptr;
  static_cast<void>(tx_mutex_info_get(mutex, nullptr, nullptr, &owner, nullptr, nullptr, nullptr));
  return owner;
}

uint32_t CurrentFlags(TX_EVENT_FLAGS_GROUP* group) noexcept {
  ULONG flags{}; // written by the kernel
  static_cast<void>(tx_event_flags_info_get(group, nullptr, &flags, nullptr, nullptr, nullptr));
  return static_cast<uint32_t>(flags);
}

// A kb priority (higher runs first) as a ThreadX one (0 is the most urgent), never as urgent as
// the timer thread, so no kb thread can delay timeouts and timer callbacks.
UINT Priority(uint32_t kb_priority) noexcept {
  constexpr uint32_t kLowest = static_cast<uint32_t>(TX_MAX_PRIORITIES) - 1U;
  constexpr uint32_t kMostUrgent = static_cast<uint32_t>(TX_TIMER_THREAD_PRIORITY) + 1U;
  const uint32_t clamped = std::min(kb_priority, kLowest - kMostUrgent);
  return static_cast<UINT>(kLowest - clamped);
}

#if defined(TX_ENABLE_STACK_CHECKING)
// Bytes of the never-written part of a thread's stack: the fill pattern from its lowest word up.
std::size_t UntouchedStack(const TX_THREAD* thread_ptr) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  const auto* const words = static_cast<const ULONG*>(thread_ptr->tx_thread_stack_start);
  const std::size_t count =
      static_cast<std::size_t>(thread_ptr->tx_thread_stack_size) / sizeof(ULONG);
  std::size_t untouched = 0U;
  while ((untouched < count) && (words[untouched] == TX_STACK_FILL)) {
    ++untouched;
  }
  return untouched * sizeof(ULONG);
}
#endif

} // namespace

namespace backend {

uint32_t TickRateHz() noexcept {
  return static_cast<uint32_t>(TX_TIMER_TICKS_PER_SECOND);
}

uint32_t TickCount() noexcept {
  return static_cast<uint32_t>(tx_time_get() & 0xFFFFFFFFUL);
}

kb::Instant Now() noexcept {
  EnterCritical();
  const uint64_t ticks = Shared().ticks.Extend(TickCount());
  ExitCritical();
  constexpr uint64_t kUsPerS = 1000000U;
  return kb::Instant{(ticks * kUsPerS) / static_cast<uint64_t>(TickRateHz())};
}

void SleepTicks(uint32_t ticks) noexcept {
  if (ticks == 0U) {
    tx_thread_relinquish();
  } else {
    static_cast<void>(tx_thread_sleep(WaitOption(ticks)));
  }
}

bool SleepUntilTick(uint32_t& previous_wake, uint32_t period_ticks) noexcept {
  // ThreadX has no delay-until: sleep the rest of the period, or report the overrun at once; a
  // wake-up after the grid point (preempted before the sleep) is an overrun too.
  const detail::DelayUntilPlan plan =
      detail::PlanDelayUntil(TickCount(), previous_wake, period_ticks);
  bool on_time = plan.on_time;
  if (on_time) {
    static_cast<void>(tx_thread_sleep(static_cast<ULONG>(plan.sleep_ticks)));
    on_time = detail::WokeOnTime(TickCount(), previous_wake, period_ticks);
  }
  previous_wake += period_ticks;
  return on_time;
}

void Yield() noexcept {
  tx_thread_relinquish();
}

void EnterCritical() noexcept {
  const UINT previous = tx_interrupt_control(TX_INT_DISABLE);
  State& s = Shared();
  if (s.critical_depth == 0U) {
    s.critical_posture = previous;
  }
  ++s.critical_depth;
}

void ExitCritical() noexcept {
  State& s = Shared();
  if (s.critical_depth > 0U) {
    --s.critical_depth;
    if (s.critical_depth == 0U) {
      static_cast<void>(tx_interrupt_control(s.critical_posture));
    }
  }
}

kb::Status MutexCreate(detail::MutexStorage& storage) noexcept {
  auto* const mutex = Construct<TX_MUTEX>(storage);
  return Publish(storage, mutex, tx_mutex_create(mutex, ObjectName(), TX_INHERIT));
}

bool MutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  auto* const mutex = Block<TX_MUTEX>(storage);
  bool taken = false;
  const TX_THREAD* const self = tx_thread_identify();
  if ((self != nullptr) && (Owner(mutex) == self)) {
    // A ThreadX mutex nests; kb::Mutex does not: the holder's second take fails
    // (backend_rules.hpp).
    const uint32_t sleep_ticks = detail::HolderRetakeSleep(timeout_ticks);
    if (sleep_ticks != 0U) {
      static_cast<void>(tx_thread_sleep(static_cast<ULONG>(sleep_ticks)));
    }
  } else {
    taken = tx_mutex_get(mutex, WaitOption(timeout_ticks)) == TX_SUCCESS;
  }
  return taken;
}

void MutexUnlock(const detail::MutexStorage& storage) noexcept {
  static_cast<void>(tx_mutex_put(Block<TX_MUTEX>(storage)));
}

void MutexDelete(detail::MutexStorage& storage) noexcept {
  static_cast<void>(tx_mutex_delete(Block<TX_MUTEX>(storage)));
  storage.handle = nullptr;
}

kb::Status RecursiveMutexCreate(detail::MutexStorage& storage) noexcept {
  return MutexCreate(storage); // a ThreadX mutex is recursive; only the lock rule differs
}

bool RecursiveMutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  return tx_mutex_get(Block<TX_MUTEX>(storage), WaitOption(timeout_ticks)) == TX_SUCCESS;
}

void RecursiveMutexUnlock(const detail::MutexStorage& storage) noexcept {
  static_cast<void>(tx_mutex_put(Block<TX_MUTEX>(storage)));
}

void RecursiveMutexDelete(detail::MutexStorage& storage) noexcept {
  static_cast<void>(tx_mutex_delete(Block<TX_MUTEX>(storage)));
  storage.handle = nullptr;
}

kb::Status SemaphoreCreate(detail::SemaphoreStorage& storage, uint32_t max_count,
                           uint32_t initial) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((max_count > 0U) && (initial <= max_count)) {
    auto* const block = Construct<SemaphoreBlock>(storage);
    block->max = static_cast<ULONG>(max_count);
    const UINT result =
        tx_semaphore_create(&block->semaphore, ObjectName(), static_cast<ULONG>(initial));
    status = Publish(storage, block, result);
  }
  return status;
}

bool SemaphoreTake(const detail::SemaphoreStorage& storage, uint32_t timeout_ticks) noexcept {
  return tx_semaphore_get(&Block<SemaphoreBlock>(storage)->semaphore, WaitOption(timeout_ticks)) ==
         TX_SUCCESS;
}

bool SemaphoreGive(const detail::SemaphoreStorage& storage) noexcept {
  auto* const block = Block<SemaphoreBlock>(storage);
  return tx_semaphore_ceiling_put(&block->semaphore, block->max) == TX_SUCCESS;
}

uint32_t SemaphoreCount(const detail::SemaphoreStorage& storage) noexcept {
  ULONG count{}; // written by the kernel
  static_cast<void>(tx_semaphore_info_get(&Block<SemaphoreBlock>(storage)->semaphore, nullptr,
                                          &count, nullptr, nullptr, nullptr));
  return static_cast<uint32_t>(count);
}

void SemaphoreDelete(detail::SemaphoreStorage& storage) noexcept {
  static_cast<void>(tx_semaphore_delete(&Block<SemaphoreBlock>(storage)->semaphore));
  storage.handle = nullptr;
}

kb::Status QueueCreate(detail::QueueStorage& storage, std::byte* buffer, std::size_t item_bytes,
                       std::size_t capacity) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  const std::size_t words = (item_bytes + sizeof(ULONG) - 1U) / sizeof(ULONG);
  if ((buffer != nullptr) && (item_bytes != 0U) && (capacity != 0U)) {
    if (words > kMaxQueueWords) {
      status = kb::Status::Unsupported; // a ThreadX message is at most 16 words
    } else {
      auto* const block = Construct<QueueBlock>(storage);
      block->item_bytes = item_bytes;
      const UINT result = tx_queue_create(&block->queue, ObjectName(), static_cast<UINT>(words),
                                          static_cast<VOID*>(buffer),
                                          static_cast<ULONG>(words * sizeof(ULONG) * capacity));
      status = Publish(storage, block, result);
    }
  }
  return status;
}

bool QueueSend(const detail::QueueStorage& storage, const void* item,
               uint32_t timeout_ticks) noexcept {
  auto* const block = Block<QueueBlock>(storage);
  Message message{};
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  const auto* const bytes = static_cast<const std::byte*>(item);
  static_cast<void>(std::copy_n(bytes, block->item_bytes, MessageBytes(message)));
  return tx_queue_send(&block->queue, message.data(), WaitOption(timeout_ticks)) == TX_SUCCESS;
}

bool QueueReceive(const detail::QueueStorage& storage, void* item,
                  uint32_t timeout_ticks) noexcept {
  auto* const block = Block<QueueBlock>(storage);
  Message message{};
  const bool received =
      tx_queue_receive(&block->queue, message.data(), WaitOption(timeout_ticks)) == TX_SUCCESS;
  if (received) {
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    auto* const out = static_cast<std::byte*>(item);
    static_cast<void>(std::copy_n(MessageBytes(message), block->item_bytes, out));
  }
  return received;
}

std::size_t QueueSize(const detail::QueueStorage& storage) noexcept {
  ULONG enqueued{}; // written by the kernel
  static_cast<void>(tx_queue_info_get(&Block<QueueBlock>(storage)->queue, nullptr, &enqueued,
                                      nullptr, nullptr, nullptr, nullptr));
  return static_cast<std::size_t>(enqueued);
}

void QueueDelete(detail::QueueStorage& storage) noexcept {
  static_cast<void>(tx_queue_delete(&Block<QueueBlock>(storage)->queue));
  storage.handle = nullptr;
}

kb::Status EventFlagsCreate(detail::EventFlagsStorage& storage) noexcept {
  auto* const group = Construct<TX_EVENT_FLAGS_GROUP>(storage);
  return Publish(storage, group, tx_event_flags_create(group, ObjectName()));
}

void EventFlagsSet(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  static_cast<void>(
      tx_event_flags_set(Block<TX_EVENT_FLAGS_GROUP>(storage), static_cast<ULONG>(bits), TX_OR));
}

void EventFlagsClear(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  const ULONG keep = ~static_cast<ULONG>(bits);
  static_cast<void>(tx_event_flags_set(Block<TX_EVENT_FLAGS_GROUP>(storage), keep, TX_AND));
}

uint32_t EventFlagsGet(const detail::EventFlagsStorage& storage) noexcept {
  return CurrentFlags(Block<TX_EVENT_FLAGS_GROUP>(storage));
}

uint32_t EventFlagsWait(const detail::EventFlagsStorage& storage, uint32_t bits, bool wait_all,
                        bool clear_on_exit, uint32_t timeout_ticks) noexcept {
  auto* const group = Block<TX_EVENT_FLAGS_GROUP>(storage);
  UINT option = wait_all ? TX_AND : TX_OR;
  if (clear_on_exit) {
    option = wait_all ? TX_AND_CLEAR : TX_OR_CLEAR;
  }
  ULONG actual{}; // written by the kernel on success
  const UINT result = tx_event_flags_get(group, static_cast<ULONG>(bits), option, &actual,
                                         WaitOption(timeout_ticks));
  // On success ThreadX reports the flags before any clear; on a timeout, read them now.
  return (result == TX_SUCCESS) ? static_cast<uint32_t>(actual) : CurrentFlags(group);
}

void EventFlagsDelete(detail::EventFlagsStorage& storage) noexcept {
  static_cast<void>(tx_event_flags_delete(Block<TX_EVENT_FLAGS_GROUP>(storage)));
  storage.handle = nullptr;
}

kb::Status TimerCreate(detail::TimerStorage& storage, const detail::TimerParams& params) noexcept {
  kb::Status status = kb::Status::Full;
  const uint32_t raw =
      kb::utils::MillisecondsToTicksCeil(Milliseconds{params.period_ms}, TickRateHz());
  const auto period = static_cast<ULONG>((raw == 0U) ? 1U : raw);
  auto* const block = Construct<TimerBlock>(storage);
  block->params = params;
  block->period_ticks = period;
  EnterCritical();
  State& s = Shared();
  std::size_t slot = 0U;
  while ((slot < s.timers.size()) && (s.timers[slot] != nullptr)) {
    ++slot;
  }
  const bool room = slot < s.timers.size();
  if (room) {
    s.timers[slot] = block; // reserved before the timer can expire
  }
  ExitCritical();
  if (room) {
    block->slot = static_cast<ULONG>(slot);
    const UINT result = tx_timer_create(&block->timer, ObjectName(), &TimerEntry, block->slot,
                                        period, params.periodic ? period : 0U, TX_NO_ACTIVATE);
    status = Publish(storage, block, result);
    if (status != kb::Status::Ok) {
      EnterCritical();
      Shared().timers[slot] = nullptr;
      ExitCritical();
    }
  }
  return status;
}

bool TimerStart(const detail::TimerStorage& storage) noexcept {
  auto* const block = Block<TimerBlock>(storage);
  // A start restarts: the whole period runs again from now.
  static_cast<void>(tx_timer_deactivate(&block->timer));
  const ULONG reschedule = block->params.periodic ? block->period_ticks : 0U;
  const bool changed =
      tx_timer_change(&block->timer, block->period_ticks, reschedule) == TX_SUCCESS;
  return changed && (tx_timer_activate(&block->timer) == TX_SUCCESS);
}

bool TimerStop(const detail::TimerStorage& storage) noexcept {
  return tx_timer_deactivate(&Block<TimerBlock>(storage)->timer) == TX_SUCCESS;
}

void TimerDelete(detail::TimerStorage& storage) noexcept {
  // tx_timer_delete also deactivates it; then its table slot is free for a new timer.
  auto* const block = Block<TimerBlock>(storage);
  static_cast<void>(tx_timer_delete(&block->timer));
  EnterCritical();
  Shared().timers[static_cast<std::size_t>(block->slot)] = nullptr;
  ExitCritical();
  storage.handle = nullptr;
}

kb::Status ThreadCreate(detail::ThreadStorage& storage,
                        const detail::ThreadParams& params) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((params.core != nullptr) && (params.core->run != nullptr) && (params.stack != nullptr) &&
      (params.stack_bytes >= static_cast<std::size_t>(TX_MINIMUM_STACK))) {
    auto* const block = Construct<ThreadBlock>(storage);
    block->core = params.core;
    block->name.fill('\0');
    const std::string_view name{(params.name != nullptr) ? params.name : "kb"};
    const std::size_t chars = std::min(name.size(), block->name.size() - 1U); // keep the terminator
    static_cast<void>(std::copy_n(name.begin(), chars, block->name.begin()));
    const UINT priority = Priority(params.priority);
    const UINT result =
        tx_thread_create(&block->thread, block->name.data(), &ThreadEntry, 0U,
                         static_cast<VOID*>(params.stack), static_cast<ULONG>(params.stack_bytes),
                         priority, priority, TX_NO_TIME_SLICE, TX_AUTO_START);
    status = Publish(storage, block, result);
  }
  return status;
}

void ThreadExit(const detail::ThreadStorage& storage) noexcept {
  // Returning from the entry function completes a ThreadX thread; ThreadRelease deletes it.
  static_cast<void>(storage);
}

void ThreadRelease(detail::ThreadStorage& storage) noexcept {
  if (storage.handle != nullptr) {
    TX_THREAD* const thread = &Block<ThreadBlock>(storage)->thread;
    static_cast<void>(tx_thread_terminate(thread));
    static_cast<void>(tx_thread_delete(thread));
    storage.handle = nullptr;
  }
}

kb::Status ThreadStackFree(const detail::ThreadStorage& storage, std::size_t& free_bytes) noexcept {
  free_bytes = 0U;
#if defined(TX_ENABLE_STACK_CHECKING)
  kb::Status status = kb::Status::NotReady;
  if (storage.handle != nullptr) {
    const TX_THREAD* const thread = &Block<ThreadBlock>(storage)->thread;
    // The deepest use is the lower of the kernel's own record and the first overwritten fill word.
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    const auto* const start = static_cast<const std::byte*>(thread->tx_thread_stack_start);
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    const auto* const highest = static_cast<const std::byte*>(thread->tx_thread_stack_highest_ptr);
    const auto recorded = static_cast<std::size_t>(highest - start);
    free_bytes = std::min(recorded, UntouchedStack(thread));
    status = kb::Status::Ok;
  }
#else
  static_cast<void>(storage);
  const kb::Status status =
      kb::Status::Unsupported; // ThreadX keeps no high-water mark without stack checking
#endif
  return status;
}

} // namespace backend
} // namespace kb::rtos
