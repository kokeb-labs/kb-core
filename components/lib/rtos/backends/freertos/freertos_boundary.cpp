/**
 * @file freertos_boundary.cpp
 * @brief The FreeRTOS backend of kb::rtos: the only kb-core file that includes FreeRTOS headers
 * (with freertos_isr.cpp). Static allocation only; no logic beyond converting types and mapping
 * results.
 *
 * Boundary file (deviations/boundary-files.yml). The FreeRTOS headers are SOUP and reached with
 * -isystem; every MISRA finding at a FreeRTOS call site here is a recorded deviation
 * (deviations/deviations.yml, DEV-KB-RTOS-*). Each native object is constructed in the kb object's
 * opaque storage; its handle is kept beside it.
 *
 * Requires configSUPPORT_STATIC_ALLOCATION = 1, configUSE_MUTEXES = 1,
 * configUSE_RECURSIVE_MUTEXES = 1, configUSE_COUNTING_SEMAPHORES = 1, configUSE_TIMERS = 1,
 * INCLUDE_uxTaskGetStackHighWaterMark = 1 and a 32-bit TickType_t. ESP-IDF provides them with
 * CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <cstddef>
#include <cstdint>
#include <limits>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/backend_rules.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static_assert(sizeof(TickType_t) == sizeof(uint32_t), "kb::rtos needs a 32-bit FreeRTOS tick");
static_assert(configSUPPORT_STATIC_ALLOCATION == 1, "kb::rtos uses the static allocation API only");

namespace kb::rtos {
namespace {

// A timer's FreeRTOS control block and the kb callback it runs; the timer ID points at this.
struct TimerBlock {
  StaticTimer_t timer{};
  detail::TimerParams params{};
};

static_assert(sizeof(StaticSemaphore_t) <= detail::kMutexBytes,
              "a FreeRTOS mutex must fit kMutexBytes");
static_assert(sizeof(StaticSemaphore_t) <= detail::kSemaphoreBytes,
              "a semaphore must fit kSemaphoreBytes");
static_assert(sizeof(StaticQueue_t) <= detail::kQueueBytes,
              "a FreeRTOS queue must fit kQueueBytes");
static_assert(sizeof(StaticEventGroup_t) <= detail::kEventFlagsBytes,
              "an event group must fit kEventFlagsBytes");
static_assert(sizeof(TimerBlock) <= detail::kTimerBytes, "a FreeRTOS timer must fit kTimerBytes");
static_assert(sizeof(StaticTask_t) <= detail::kThreadBytes,
              "a FreeRTOS task must fit kThreadBytes");

// Raw kb-owned bytes as the FreeRTOS buffer type the static API expects. MISRA C++:2023 forbids
// reinterpret_cast (Rule 8.2.5), so the conversion goes through void*: one place, a recorded
// deviation (deviations/deviations.yml).
template <typename T>
T* AsBuffer(std::byte* bytes) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<T*>(static_cast<void*>(bytes)); // NOLINT(bugprone-casting-through-void)
}

// The address of the opaque bytes as the FreeRTOS buffer type.
template <typename T, std::size_t Bytes>
T* Buffer(detail::OpaqueStorage<Bytes>& storage) noexcept {
  return AsBuffer<T>(storage.bytes.data());
}

SemaphoreHandle_t Semaphore(void* handle) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<SemaphoreHandle_t>(handle);
}

QueueHandle_t Queue(void* handle) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<QueueHandle_t>(handle);
}

EventGroupHandle_t Group(void* handle) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<EventGroupHandle_t>(handle);
}

TimerHandle_t TimerOf(void* handle) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<TimerHandle_t>(handle);
}

// The FreeRTOS task function of every kb thread: the parameter is the ThreadCore that ThreadCreate
// passed.
void TaskEntry(void* parameter) {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  auto* core = static_cast<detail::ThreadCore*>(parameter);
  core->run(*core);
}

void TimerTrampoline(TimerHandle_t timer) {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  const auto* block = static_cast<const TimerBlock*>(pvTimerGetTimerID(timer));
  block->params.callback(block->params.context);
}

#if defined(ESP_PLATFORM)
portMUX_TYPE g_critical = portMUX_INITIALIZER_UNLOCKED;
#endif

// The 64-bit extension of the 32-bit tick counter (backend_rules.hpp), read by Now().
detail::TickExtender& Extension() noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-002)
  static detail::TickExtender extension{};
  return extension;
}

} // namespace

namespace backend {

uint32_t TickRateHz() noexcept {
  return static_cast<uint32_t>(configTICK_RATE_HZ);
}

uint32_t TickCount() noexcept {
  return static_cast<uint32_t>(xTaskGetTickCount());
}

kb::Instant Now() noexcept {
  EnterCritical();
  const uint64_t ticks = Extension().Extend(TickCount());
  ExitCritical();
  constexpr uint64_t kUsPerS = 1000000U;
  return kb::Instant{(ticks * kUsPerS) / static_cast<uint64_t>(TickRateHz())};
}

void SleepTicks(uint32_t ticks) noexcept {
  if (ticks == 0U) {
    taskYIELD();
  } else {
    vTaskDelay(static_cast<TickType_t>(ticks));
  }
}

bool SleepUntilTick(uint32_t& previous_wake, uint32_t period_ticks) noexcept {
  auto wake = static_cast<TickType_t>(previous_wake);
  const BaseType_t delayed = xTaskDelayUntil(&wake, static_cast<TickType_t>(period_ticks));
  previous_wake = static_cast<uint32_t>(wake);
  return delayed == pdTRUE;
}

void Yield() noexcept {
  taskYIELD();
}

void EnterCritical() noexcept {
#if defined(ESP_PLATFORM)
  taskENTER_CRITICAL(&g_critical);
#else
  taskENTER_CRITICAL();
#endif
}

void ExitCritical() noexcept {
#if defined(ESP_PLATFORM)
  taskEXIT_CRITICAL(&g_critical);
#else
  taskEXIT_CRITICAL();
#endif
}

kb::Status MutexCreate(detail::MutexStorage& storage) noexcept {
  SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(Buffer<StaticSemaphore_t>(storage));
  storage.handle = handle;
  return (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
}

kb::Status RecursiveMutexCreate(detail::MutexStorage& storage) noexcept {
  SemaphoreHandle_t handle =
      xSemaphoreCreateRecursiveMutexStatic(Buffer<StaticSemaphore_t>(storage));
  storage.handle = handle;
  return (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
}

bool RecursiveMutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  return xSemaphoreTakeRecursive(Semaphore(storage.handle),
                                 static_cast<TickType_t>(timeout_ticks)) == pdTRUE;
}

void RecursiveMutexUnlock(const detail::MutexStorage& storage) noexcept {
  static_cast<void>(xSemaphoreGiveRecursive(Semaphore(storage.handle)));
}

bool MutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept {
  return xSemaphoreTake(Semaphore(storage.handle), static_cast<TickType_t>(timeout_ticks)) ==
         pdTRUE;
}

void MutexUnlock(const detail::MutexStorage& storage) noexcept {
  static_cast<void>(xSemaphoreGive(Semaphore(storage.handle)));
}

kb::Status SemaphoreCreate(detail::SemaphoreStorage& storage, uint32_t max_count,
                           uint32_t initial) noexcept {
  SemaphoreHandle_t handle = xSemaphoreCreateCountingStatic(static_cast<UBaseType_t>(max_count),
                                                            static_cast<UBaseType_t>(initial),
                                                            Buffer<StaticSemaphore_t>(storage));
  storage.handle = handle;
  return (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
}

bool SemaphoreTake(const detail::SemaphoreStorage& storage, uint32_t timeout_ticks) noexcept {
  return xSemaphoreTake(Semaphore(storage.handle), static_cast<TickType_t>(timeout_ticks)) ==
         pdTRUE;
}

bool SemaphoreGive(const detail::SemaphoreStorage& storage) noexcept {
  return xSemaphoreGive(Semaphore(storage.handle)) == pdTRUE;
}

uint32_t SemaphoreCount(const detail::SemaphoreStorage& storage) noexcept {
  return static_cast<uint32_t>(uxSemaphoreGetCount(Semaphore(storage.handle)));
}

kb::Status QueueCreate(detail::QueueStorage& storage, std::byte* buffer, std::size_t item_bytes,
                       std::size_t capacity) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((buffer != nullptr) && (item_bytes > 0U) && (capacity > 0U)) {
    QueueHandle_t handle =
        xQueueCreateStatic(static_cast<UBaseType_t>(capacity), static_cast<UBaseType_t>(item_bytes),
                           AsBuffer<uint8_t>(buffer), Buffer<StaticQueue_t>(storage));
    storage.handle = handle;
    status = (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
  }
  return status;
}

bool QueueSend(const detail::QueueStorage& storage, const void* item,
               uint32_t timeout_ticks) noexcept {
  return xQueueSend(Queue(storage.handle), item, static_cast<TickType_t>(timeout_ticks)) == pdTRUE;
}

bool QueueReceive(const detail::QueueStorage& storage, void* item,
                  uint32_t timeout_ticks) noexcept {
  return xQueueReceive(Queue(storage.handle), item, static_cast<TickType_t>(timeout_ticks)) ==
         pdTRUE;
}

std::size_t QueueSize(const detail::QueueStorage& storage) noexcept {
  return static_cast<std::size_t>(uxQueueMessagesWaiting(Queue(storage.handle)));
}

kb::Status EventFlagsCreate(detail::EventFlagsStorage& storage) noexcept {
  EventGroupHandle_t handle = xEventGroupCreateStatic(Buffer<StaticEventGroup_t>(storage));
  storage.handle = handle;
  return (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
}

void EventFlagsSet(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  static_cast<void>(xEventGroupSetBits(Group(storage.handle), static_cast<EventBits_t>(bits)));
}

void EventFlagsClear(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  static_cast<void>(xEventGroupClearBits(Group(storage.handle), static_cast<EventBits_t>(bits)));
}

uint32_t EventFlagsGet(const detail::EventFlagsStorage& storage) noexcept {
  return static_cast<uint32_t>(xEventGroupGetBits(Group(storage.handle)));
}

uint32_t EventFlagsWait(const detail::EventFlagsStorage& storage, uint32_t bits, bool wait_all,
                        bool clear_on_exit, uint32_t timeout_ticks) noexcept {
  const EventBits_t seen = xEventGroupWaitBits(
      Group(storage.handle), static_cast<EventBits_t>(bits), clear_on_exit ? pdTRUE : pdFALSE,
      wait_all ? pdTRUE : pdFALSE, static_cast<TickType_t>(timeout_ticks));
  return static_cast<uint32_t>(seen);
}

kb::Status TimerCreate(detail::TimerStorage& storage, const detail::TimerParams& params) noexcept {
  auto* block = Buffer<TimerBlock>(storage);
  block->params = params;
  // The period in ticks, rounded up and at least one (milliseconds are ticks only at 1000 Hz).
  const uint32_t raw =
      kb::utils::MillisecondsToTicksCeil(Milliseconds{params.period_ms}, TickRateHz());
  const uint32_t period = (raw == 0U) ? 1U : raw;
  TimerHandle_t handle =
      xTimerCreateStatic("kb", static_cast<TickType_t>(period), params.periodic ? pdTRUE : pdFALSE,
                         block, &TimerTrampoline, &block->timer);
  storage.handle = handle;
  return (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
}

bool TimerStart(const detail::TimerStorage& storage) noexcept {
  return xTimerStart(TimerOf(storage.handle), 0U) == pdPASS;
}

bool TimerStop(const detail::TimerStorage& storage) noexcept {
  return xTimerStop(TimerOf(storage.handle), 0U) == pdPASS;
}

kb::Status ThreadCreate(detail::ThreadStorage& storage,
                        const detail::ThreadParams& params) noexcept {
  kb::Status status = kb::Status::InvalidArgument;
  if ((params.core != nullptr) && (params.core->run != nullptr) && (params.stack != nullptr) &&
      (params.stack_bytes >= sizeof(StackType_t))) {
    constexpr uint32_t kTopPriority = static_cast<uint32_t>(configMAX_PRIORITIES) - 1U;
    const uint32_t priority = (params.priority > kTopPriority) ? kTopPriority : params.priority;
    const auto depth = static_cast<uint32_t>(params.stack_bytes / sizeof(StackType_t));
    TaskHandle_t handle = xTaskCreateStatic(
        &TaskEntry, params.name, depth, params.core, static_cast<UBaseType_t>(priority),
        AsBuffer<StackType_t>(params.stack), Buffer<StaticTask_t>(storage));
    storage.handle = handle;
    status = (handle != nullptr) ? kb::Status::Ok : kb::Status::Fault;
  }
  return status;
}

void ThreadExit(const detail::ThreadStorage& storage) noexcept {
  static_cast<void>(storage);
  vTaskDelete(nullptr);
}

void ThreadRelease(detail::ThreadStorage& storage) noexcept {
  storage.handle = nullptr;
}

kb::Status ThreadStackFree(const detail::ThreadStorage& storage, std::size_t& free_bytes) noexcept {
  kb::Status status = kb::Status::NotReady;
  free_bytes = 0U;
  if (storage.handle != nullptr) {
    // codeql::misra_deviation_next_line(DEV-KBC-001)
    auto* const task = static_cast<TaskHandle_t>(storage.handle);
    free_bytes = static_cast<std::size_t>(uxTaskGetStackHighWaterMark(task)) * sizeof(StackType_t);
    status = kb::Status::Ok;
  }
  return status;
}

} // namespace backend
} // namespace kb::rtos
