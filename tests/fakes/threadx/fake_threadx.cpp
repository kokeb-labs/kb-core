// The fake ThreadX kernel (see tx_api.h): single-threaded semantics of the calls kb-core's ThreadX
// backend makes. A blocking call that cannot succeed advances the clock by its wait option and
// fails; TX_WAIT_FOREVER fails at once. Objects live in the control blocks the caller passes, as
// with the real kernel. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fake_threadx.hpp"
#include "tx_api.h"

namespace {

struct Kernel {
  ULONG tick{0U};
  bool fail_next{false};
  UINT posture{TX_INT_ENABLE};
  std::array<TX_THREAD*, 8> threads{};
  std::size_t thread_count{0U};
  std::array<TX_TIMER*, 40> timers{};
  std::size_t timer_count{0U};
  TX_THREAD test_thread{};
  TX_THREAD* current{nullptr}; // null: the test thread
  bool outside{false};         // true: not in a thread (an ISR, start-up)
  uint32_t terminates{0U};
  uint32_t deletes{0U};
  uint32_t relinquishes{0U};
  uint32_t sleeps{0U};
  uint32_t semaphore_creates{0U};
  uint32_t overshoot{0U};
  bool fail_thread{false};
  bool fail_activate{false};
};

Kernel& K() {
  static Kernel k{};
  return k;
}

// Every successful tx_timer_create of the process: the backend never deletes a timer.
uint32_t& TimersEver() {
  static uint32_t count = 0U;
  return count;
}

TX_THREAD* Current() {
  TX_THREAD* thread = nullptr;
  if (!K().outside) {
    thread = (K().current != nullptr) ? K().current : &K().test_thread;
  }
  return thread;
}

bool Fail() {
  const bool fail = K().fail_next;
  K().fail_next = false;
  return fail;
}

void WaitOut(ULONG ticks) {
  if (ticks != TX_WAIT_FOREVER) {
    kb_fake_threadx::AdvanceTicks(ticks);
  }
}

} // namespace

namespace kb_fake_threadx {

void Reset() {
  K() = Kernel{};
}
void SetTick(ULONG tick) {
  K().tick = tick;
}
void AdvanceTicks(uint32_t ticks) {
  for (uint32_t i = 0U; i < ticks; ++i) {
    ++K().tick;
    for (std::size_t t = 0U; t < K().timer_count; ++t) {
      TX_TIMER* timer = K().timers[t];
      if (timer->active && (--timer->remaining == 0U)) {
        timer->active = timer->reschedule != 0U;
        timer->remaining = timer->reschedule;
        timer->function(timer->input);
      }
    }
  }
}
void FailNextThreadCreate() {
  K().fail_thread = true;
}
void FailNextActivate() {
  K().fail_activate = true;
}
void FailNextCall() {
  K().fail_next = true;
}
std::size_t ThreadCount() {
  return K().thread_count;
}
TX_THREAD* Thread(std::size_t index) {
  return K().threads[index];
}
void RunThread(std::size_t index) {
  TX_THREAD* const previous = K().current;
  const bool outside = K().outside;
  TX_THREAD* const thread = K().threads[index];
  K().current = thread;
  K().outside = false;
  thread->entry(thread->entry_input);
  K().current = previous;
  K().outside = outside;
}
void SetCurrent(TX_THREAD* thread) {
  K().current = thread;
  K().outside = thread == nullptr;
}
TX_THREAD* TestThread() {
  return &K().test_thread;
}
uint32_t Terminates() {
  return K().terminates;
}
uint32_t Deletes() {
  return K().deletes;
}
uint32_t Relinquishes() {
  return K().relinquishes;
}
void SetSleepOvershoot(uint32_t ticks) {
  K().overshoot = ticks;
}
uint32_t Sleeps() {
  return K().sleeps;
}
UINT Posture() {
  return K().posture;
}
uint32_t TimersCreatedEver() {
  return TimersEver();
}
uint32_t SemaphoreCreates() {
  return K().semaphore_creates;
}
std::size_t TimerCount() {
  return K().timer_count;
}
TX_TIMER* Timer(std::size_t index) {
  return K().timers[index];
}

} // namespace kb_fake_threadx

UINT tx_interrupt_control(UINT new_posture) {
  const UINT old = K().posture;
  K().posture = new_posture;
  return old;
}
ULONG tx_time_get() {
  return K().tick;
}
TX_THREAD* tx_thread_identify() {
  return Current();
}
VOID tx_thread_relinquish() {
  ++K().relinquishes;
}
UINT tx_thread_sleep(ULONG timer_ticks) {
  ++K().sleeps;
  kb_fake_threadx::AdvanceTicks(timer_ticks + K().overshoot);
  K().overshoot = 0U;
  return TX_SUCCESS;
}
UINT tx_thread_create(TX_THREAD* thread_ptr, CHAR* name_ptr, VOID (*entry_function)(ULONG),
                      ULONG entry_input, VOID* stack_start, ULONG stack_size, UINT priority,
                      UINT preempt_threshold, ULONG time_slice, UINT auto_start) {
  const bool fail_thread = K().fail_thread;
  K().fail_thread = false;
  if (Fail() || fail_thread || (K().thread_count >= K().threads.size())) {
    return TX_THREAD_ERROR;
  }
  *thread_ptr = TX_THREAD{};
  thread_ptr->tx_thread_stack_start = stack_start;
  thread_ptr->tx_thread_stack_size = stack_size;
  // Like the kernel: fill the stack, then put a 64-byte initial frame at its top.
  auto* const words = static_cast<ULONG*>(stack_start);
  const std::size_t count = stack_size / sizeof(ULONG);
  for (std::size_t i = 0U; i < count; ++i) {
    words[i] = TX_STACK_FILL;
  }
  constexpr std::size_t kFrameWords = 16U;
  const std::size_t frame = (count > kFrameWords) ? (count - kFrameWords) : 0U;
  for (std::size_t i = frame; i < count; ++i) {
    words[i] = 0U;
  }
  thread_ptr->tx_thread_stack_highest_ptr = &words[frame];
  thread_ptr->entry = entry_function;
  thread_ptr->entry_input = entry_input;
  thread_ptr->name = name_ptr;
  thread_ptr->priority = priority;
  thread_ptr->preempt_threshold = preempt_threshold;
  thread_ptr->time_slice = time_slice;
  thread_ptr->auto_start = auto_start;
  thread_ptr->index = static_cast<uint32_t>(K().thread_count);
  K().threads[K().thread_count] = thread_ptr;
  ++K().thread_count;
  return TX_SUCCESS;
}
UINT tx_thread_terminate(TX_THREAD* thread_ptr) {
  static_cast<void>(thread_ptr);
  ++K().terminates;
  return TX_SUCCESS;
}
UINT tx_thread_delete(TX_THREAD* thread_ptr) {
  static_cast<void>(thread_ptr);
  ++K().deletes;
  return TX_SUCCESS;
}

UINT tx_mutex_create(TX_MUTEX* mutex_ptr, CHAR* name_ptr, UINT inherit) {
  static_cast<void>(name_ptr);
  if (Fail()) {
    return TX_NOT_AVAILABLE;
  }
  *mutex_ptr = TX_MUTEX{};
  mutex_ptr->inherit = inherit;
  return TX_SUCCESS;
}
UINT tx_mutex_get(TX_MUTEX* mutex_ptr, ULONG wait_option) {
  if ((mutex_ptr->owner == nullptr) || (mutex_ptr->owner == Current())) {
    mutex_ptr->owner = Current();
    ++mutex_ptr->count;
    return TX_SUCCESS;
  }
  WaitOut(wait_option);
  return TX_NOT_AVAILABLE;
}
UINT tx_mutex_put(TX_MUTEX* mutex_ptr) {
  if ((mutex_ptr->owner == nullptr) || (mutex_ptr->owner != Current())) {
    return TX_NOT_OWNED;
  }
  --mutex_ptr->count;
  if (mutex_ptr->count == 0U) {
    mutex_ptr->owner = nullptr;
  }
  return TX_SUCCESS;
}
UINT tx_mutex_info_get(TX_MUTEX* mutex_ptr, CHAR** name, ULONG* count, TX_THREAD** owner,
                       TX_THREAD** first_suspended, ULONG* suspended_count, TX_MUTEX** next_mutex) {
  static_cast<void>(name);
  static_cast<void>(first_suspended);
  static_cast<void>(suspended_count);
  static_cast<void>(next_mutex);
  if (count != nullptr) {
    *count = mutex_ptr->count;
  }
  if (owner != nullptr) {
    *owner = mutex_ptr->owner;
  }
  return TX_SUCCESS;
}

UINT tx_semaphore_create(TX_SEMAPHORE* semaphore_ptr, CHAR* name_ptr, ULONG initial_count) {
  static_cast<void>(name_ptr);
  if (Fail()) {
    return TX_NOT_AVAILABLE;
  }
  semaphore_ptr->count = initial_count;
  ++K().semaphore_creates;
  return TX_SUCCESS;
}
UINT tx_semaphore_get(TX_SEMAPHORE* semaphore_ptr, ULONG wait_option) {
  if (semaphore_ptr->count == 0U) {
    WaitOut(wait_option);
    return TX_NO_INSTANCE;
  }
  --semaphore_ptr->count;
  return TX_SUCCESS;
}
UINT tx_semaphore_ceiling_put(TX_SEMAPHORE* semaphore_ptr, ULONG ceiling) {
  if (semaphore_ptr->count >= ceiling) {
    return TX_CEILING_EXCEEDED;
  }
  ++semaphore_ptr->count;
  return TX_SUCCESS;
}
UINT tx_semaphore_info_get(TX_SEMAPHORE* semaphore_ptr, CHAR** name, ULONG* current_value,
                           TX_THREAD** first_suspended, ULONG* suspended_count,
                           TX_SEMAPHORE** next_semaphore) {
  static_cast<void>(name);
  static_cast<void>(first_suspended);
  static_cast<void>(suspended_count);
  static_cast<void>(next_semaphore);
  *current_value = semaphore_ptr->count;
  return TX_SUCCESS;
}

UINT tx_queue_create(TX_QUEUE* queue_ptr, CHAR* name_ptr, UINT message_size, VOID* queue_start,
                     ULONG queue_size) {
  static_cast<void>(name_ptr);
  if (Fail()) {
    return TX_NOT_AVAILABLE;
  }
  *queue_ptr = TX_QUEUE{};
  queue_ptr->start = static_cast<ULONG*>(queue_start);
  queue_ptr->words = message_size;
  queue_ptr->capacity = queue_size / (message_size * static_cast<ULONG>(sizeof(ULONG)));
  return TX_SUCCESS;
}
UINT tx_queue_send(TX_QUEUE* queue_ptr, VOID* source_ptr, ULONG wait_option) {
  if (queue_ptr->count >= queue_ptr->capacity) {
    WaitOut(wait_option);
    return TX_QUEUE_FULL;
  }
  const ULONG slot = (queue_ptr->head + queue_ptr->count) % queue_ptr->capacity;
  std::memcpy(&queue_ptr->start[slot * queue_ptr->words], source_ptr,
              queue_ptr->words * sizeof(ULONG));
  ++queue_ptr->count;
  return TX_SUCCESS;
}
UINT tx_queue_receive(TX_QUEUE* queue_ptr, VOID* destination_ptr, ULONG wait_option) {
  if (queue_ptr->count == 0U) {
    WaitOut(wait_option);
    return TX_QUEUE_EMPTY;
  }
  std::memcpy(destination_ptr, &queue_ptr->start[queue_ptr->head * queue_ptr->words],
              queue_ptr->words * sizeof(ULONG));
  queue_ptr->head = (queue_ptr->head + 1U) % queue_ptr->capacity;
  --queue_ptr->count;
  return TX_SUCCESS;
}
UINT tx_queue_info_get(TX_QUEUE* queue_ptr, CHAR** name, ULONG* enqueued, ULONG* available_storage,
                       TX_THREAD** first_suspended, ULONG* suspended_count, TX_QUEUE** next_queue) {
  static_cast<void>(name);
  static_cast<void>(available_storage);
  static_cast<void>(first_suspended);
  static_cast<void>(suspended_count);
  static_cast<void>(next_queue);
  *enqueued = queue_ptr->count;
  return TX_SUCCESS;
}

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP* group_ptr, CHAR* name_ptr) {
  static_cast<void>(name_ptr);
  if (Fail()) {
    return TX_NOT_AVAILABLE;
  }
  group_ptr->flags = 0U;
  return TX_SUCCESS;
}
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP* group_ptr, ULONG flags_to_set, UINT set_option) {
  if (set_option == TX_AND) {
    group_ptr->flags &= flags_to_set;
  } else {
    group_ptr->flags |= flags_to_set;
  }
  return TX_SUCCESS;
}
UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP* group_ptr, ULONG requested_flags, UINT get_option,
                        ULONG* actual_flags_ptr, ULONG wait_option) {
  const bool all = (get_option == TX_AND) || (get_option == TX_AND_CLEAR);
  const bool clear = (get_option == TX_AND_CLEAR) || (get_option == TX_OR_CLEAR);
  const ULONG seen = group_ptr->flags;
  const bool met =
      all ? ((seen & requested_flags) == requested_flags) : ((seen & requested_flags) != 0U);
  if (!met) {
    WaitOut(wait_option);
    return TX_NO_EVENTS;
  }
  *actual_flags_ptr = seen;
  if (clear) {
    group_ptr->flags &= ~requested_flags;
  }
  return TX_SUCCESS;
}
UINT tx_event_flags_info_get(TX_EVENT_FLAGS_GROUP* group_ptr, CHAR** name, ULONG* current_flags,
                             TX_THREAD** first_suspended, ULONG* suspended_count,
                             TX_EVENT_FLAGS_GROUP** next_group) {
  static_cast<void>(name);
  static_cast<void>(first_suspended);
  static_cast<void>(suspended_count);
  static_cast<void>(next_group);
  *current_flags = group_ptr->flags;
  return TX_SUCCESS;
}

UINT tx_timer_create(TX_TIMER* timer_ptr, CHAR* name_ptr, VOID (*expiration_function)(ULONG),
                     ULONG expiration_input, ULONG initial_ticks, ULONG reschedule_ticks,
                     UINT auto_activate) {
  static_cast<void>(name_ptr);
  if (Fail() || (K().timer_count >= K().timers.size())) {
    return TX_NOT_AVAILABLE;
  }
  *timer_ptr = TX_TIMER{};
  timer_ptr->function = expiration_function;
  timer_ptr->input = expiration_input;
  timer_ptr->remaining = initial_ticks;
  timer_ptr->reschedule = reschedule_ticks;
  timer_ptr->active = auto_activate != TX_NO_ACTIVATE;
  K().timers[K().timer_count] = timer_ptr;
  ++K().timer_count;
  ++TimersEver();
  return TX_SUCCESS;
}
UINT tx_timer_activate(TX_TIMER* timer_ptr) {
  const bool fail_activate = K().fail_activate;
  K().fail_activate = false;
  if (Fail() || fail_activate) {
    return TX_NOT_AVAILABLE;
  }
  timer_ptr->active = true;
  return TX_SUCCESS;
}
UINT tx_timer_deactivate(TX_TIMER* timer_ptr) {
  timer_ptr->active = false;
  return TX_SUCCESS;
}
UINT tx_timer_change(TX_TIMER* timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks) {
  if (Fail()) {
    return TX_NOT_AVAILABLE;
  }
  timer_ptr->remaining = initial_ticks;
  timer_ptr->reschedule = reschedule_ticks;
  return TX_SUCCESS;
}
