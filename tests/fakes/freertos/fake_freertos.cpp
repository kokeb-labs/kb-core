// The fake FreeRTOS kernel (see FreeRTOS.h): single-threaded semantics of the calls kb-core's
// FreeRTOS backend makes. A blocking call that cannot succeed advances the clock by its timeout and
// fails; portMAX_DELAY fails at once. Objects live in the static buffers the caller passes, as with
// the real static API. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "fake_freertos.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

struct FakeQueue {
  uint8_t* storage{nullptr};
  UBaseType_t item_size{0U};
  UBaseType_t length{0U};
  UBaseType_t head{0U};
  UBaseType_t count{0U};
  UBaseType_t depth{0U}; // recursive mutex: levels the (only) caller holds
  bool is_mutex{false};
};
struct FakeGroup {
  EventBits_t bits{0U};
};
struct FakeTimer {
  TimerCallbackFunction_t callback{nullptr};
  void* id{nullptr};
  TickType_t period{1U};
  TickType_t remaining{0U};
  bool reload{false};
  bool active{false};
};
struct FakeTask {
  std::size_t index{0U};
};

static_assert(sizeof(FakeQueue) <= sizeof(StaticQueue_t), "fake queue fits");
static_assert(sizeof(FakeQueue) <= sizeof(StaticSemaphore_t), "fake semaphore fits");
static_assert(sizeof(FakeGroup) <= sizeof(StaticEventGroup_t), "fake group fits");
static_assert(sizeof(FakeTimer) <= sizeof(StaticTimer_t), "fake timer fits");
static_assert(sizeof(FakeTask) <= sizeof(StaticTask_t), "fake task fits");

namespace {

struct Kernel {
  TickType_t tick{0U};
  bool fail_next{false};
  bool woken{false};
  std::array<kb_fake_freertos::TaskRecord, 8> tasks{};
  std::size_t task_count{0U};
  std::array<FakeTimer*, 16> timers{};
  std::size_t timer_count{0U};
  uint32_t deletes{0U};
  uint32_t queue_deletes{0U};
  uint32_t group_deletes{0U};
  uint32_t yields{0U};
  uint32_t isr_yields{0U};
  int critical{0};
  TickType_t timer_ticks{0xFFFFFFFFUL};
  UBaseType_t high_water{0U};
};

Kernel& K() {
  static Kernel k{};
  return k;
}

bool Fail() {
  const bool fail = K().fail_next;
  K().fail_next = false;
  return fail;
}

void WaitOut(TickType_t ticks) {
  if (ticks != portMAX_DELAY) {
    kb_fake_freertos::AdvanceTicks(ticks);
  }
}

FakeQueue* MakeQueue(void* buffer, uint8_t* storage, UBaseType_t item, UBaseType_t length,
                     bool mutex) {
  FakeQueue* q = nullptr;
  if (!Fail()) {
    q = ::new (buffer) FakeQueue{};
    q->storage = storage;
    q->item_size = item;
    q->length = length;
    q->is_mutex = mutex;
  }
  return q;
}

BaseType_t Put(FakeQueue* q, const void* item) {
  if (q->count >= q->length) {
    return pdFALSE;
  }
  if (q->item_size > 0U) {
    std::memcpy(&q->storage[((q->head + q->count) % q->length) * q->item_size], item, q->item_size);
  }
  ++q->count;
  return pdTRUE;
}

BaseType_t Woken(BaseType_t* woken) {
  if (woken != nullptr) {
    *woken = K().woken ? pdTRUE : pdFALSE;
  }
  return pdTRUE;
}

} // namespace

namespace kb_fake_freertos {

void Reset() {
  K() = Kernel{};
}
void SetTick(TickType_t tick) {
  K().tick = tick;
}
void AdvanceTicks(uint32_t ticks) {
  for (uint32_t i = 0U; i < ticks; ++i) {
    ++K().tick;
    for (std::size_t t = 0U; t < K().timer_count; ++t) {
      FakeTimer* timer = K().timers[t];
      if (timer->active && (--timer->remaining == 0U)) {
        timer->remaining = timer->period;
        timer->active = timer->reload;
        timer->callback(timer);
      }
    }
  }
}
void FailNextCreate() {
  K().fail_next = true;
}
std::size_t TaskCount() {
  return K().task_count;
}
TaskRecord Task(std::size_t index) {
  return K().tasks[index];
}
void RunTask(std::size_t index) {
  const TaskRecord r = K().tasks[index];
  r.code(r.parameters);
}
uint32_t TaskDeletes() {
  return K().deletes;
}
uint32_t QueueDeletes() {
  return K().queue_deletes;
}
uint32_t GroupDeletes() {
  return K().group_deletes;
}
uint32_t Yields() {
  return K().yields;
}
uint32_t YieldsFromIsr() {
  return K().isr_yields;
}
int CriticalDepth() {
  return K().critical;
}
void WakeHigherPriorityFromIsr(bool woken) {
  K().woken = woken;
}
TickType_t LastTimerCommandTicks() {
  return K().timer_ticks;
}
TickType_t TimerPeriod(std::size_t index) {
  return K().timers[index]->period;
}
void SetStackHighWater(UBaseType_t words) {
  K().high_water = words;
}

} // namespace kb_fake_freertos

void FakeYield() {
  ++K().yields;
}
void FakeYieldFromIsr(BaseType_t woken) {
  if (woken != pdFALSE) {
    ++K().isr_yields;
  }
}
void FakeEnterCritical() {
  ++K().critical;
}
void FakeExitCritical() {
  --K().critical;
}

TickType_t xTaskGetTickCount() {
  return K().tick;
}
void vTaskDelay(TickType_t ticks) {
  kb_fake_freertos::AdvanceTicks(ticks);
}
BaseType_t xTaskDelayUntil(TickType_t* previous_wake, TickType_t increment) {
  const TickType_t target = *previous_wake + increment;
  const TickType_t elapsed = K().tick - *previous_wake;
  const bool delay = elapsed < increment;
  if (delay) {
    kb_fake_freertos::AdvanceTicks(target - K().tick);
  }
  *previous_wake = target;
  return delay ? pdTRUE : pdFALSE;
}
TaskHandle_t xTaskCreateStatic(TaskFunction_t code, const char* name, uint32_t depth,
                               void* parameters, UBaseType_t priority, StackType_t* stack,
                               StaticTask_t* buffer) {
  static_cast<void>(stack);
  if (Fail() || (K().task_count >= K().tasks.size())) {
    return nullptr;
  }
  FakeTask* task = ::new (static_cast<void*>(buffer)) FakeTask{};
  task->index = K().task_count;
  K().tasks[K().task_count] = kb_fake_freertos::TaskRecord{code, parameters, depth, priority, name};
  ++K().task_count;
  return task;
}
void vTaskDelete(TaskHandle_t task) {
  if (task == nullptr) {
    ++K().deletes;
  }
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
  static_cast<void>(task);
  return K().high_water;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t* storage,
                                 StaticQueue_t* buffer) {
  return MakeQueue(buffer, storage, item_size, length, false);
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks) {
  const BaseType_t ok = Put(queue, item);
  if (ok == pdFALSE) {
    WaitOut(ticks);
  }
  return ok;
}
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void* item, BaseType_t* woken) {
  static_cast<void>(Woken(woken));
  return Put(queue, item);
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticks) {
  if (queue->count == 0U) {
    WaitOut(ticks);
    return pdFALSE;
  }
  if (queue->item_size > 0U) {
    std::memcpy(item, &queue->storage[queue->head * queue->item_size], queue->item_size);
  }
  queue->head = (queue->head + 1U) % queue->length;
  --queue->count;
  return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
  return queue->count;
}
void vQueueDelete(QueueHandle_t queue) {
  queue->~FakeQueue();
  ++K().queue_deletes;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buffer) {
  FakeQueue* q = MakeQueue(buffer, nullptr, 0U, 1U, true);
  if (q != nullptr) {
    q->count = 1U;
  }
  return q;
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max, UBaseType_t initial,
                                                 StaticSemaphore_t* buffer) {
  FakeQueue* q = MakeQueue(buffer, nullptr, 0U, max, false);
  if (q != nullptr) {
    q->count = initial;
  }
  return q;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) {
  return xQueueReceive(semaphore, nullptr, ticks);
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  return Put(semaphore, nullptr);
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t* woken) {
  static_cast<void>(Woken(woken));
  return Put(semaphore, nullptr);
}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore) {
  return semaphore->count;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* buffer) {
  return xSemaphoreCreateMutexStatic(buffer);
}
// One context: the caller is the holder whenever the mutex is held, so a take nests.
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t ticks) {
  static_cast<void>(ticks);
  if (mutex->depth == 0U) {
    mutex->count = 0U;
  }
  ++mutex->depth;
  return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  if (mutex->depth == 0U) {
    return pdFALSE;
  }
  --mutex->depth;
  if (mutex->depth == 0U) {
    mutex->count = 1U;
  }
  return pdTRUE;
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* buffer) {
  return Fail() ? nullptr : ::new (static_cast<void*>(buffer)) FakeGroup{};
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
  group->bits |= bits;
  return group->bits;
}
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t group, EventBits_t bits,
                                     BaseType_t* woken) {
  static_cast<void>(Woken(woken));
  group->bits |= bits;
  return pdPASS;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
  const EventBits_t before = group->bits;
  group->bits &= ~bits;
  return before;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) {
  return group->bits;
}
void vEventGroupDelete(EventGroupHandle_t group) {
  group->~FakeGroup();
  ++K().group_deletes;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all, TickType_t ticks) {
  const EventBits_t seen = group->bits;
  const bool met = (wait_all != pdFALSE) ? ((seen & bits) == bits) : ((seen & bits) != 0U);
  if (met && (clear_on_exit != pdFALSE)) {
    group->bits &= ~bits;
  }
  if (!met) {
    WaitOut(ticks);
    return group->bits;
  }
  return seen;
}

TimerHandle_t xTimerCreateStatic(const char* name, TickType_t period, BaseType_t auto_reload,
                                 void* id, TimerCallbackFunction_t callback,
                                 StaticTimer_t* buffer) {
  static_cast<void>(name);
  if (Fail() || (K().timer_count >= K().timers.size())) {
    return nullptr;
  }
  FakeTimer* t = ::new (static_cast<void*>(buffer)) FakeTimer{};
  t->callback = callback;
  t->id = id;
  t->period = period;
  t->reload = auto_reload != pdFALSE;
  K().timers[K().timer_count] = t;
  ++K().timer_count;
  return t;
}
void* pvTimerGetTimerID(TimerHandle_t timer) {
  return timer->id;
}
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks) {
  K().timer_ticks = ticks;
  timer->remaining = timer->period;
  timer->active = true;
  return pdPASS;
}
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks) {
  K().timer_ticks = ticks;
  timer->active = false;
  return pdPASS;
}
