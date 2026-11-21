/**
 * @file freertos_isr.cpp
 * @brief The interrupt-safe calls of the FreeRTOS backend (`...FromIsr`): each gives, sends or sets
 * from an interrupt handler and requests a context switch on exit when a higher-priority task was
 * woken.
 *
 * Boundary file (deviations/boundary-files.yml); see freertos_boundary.cpp for the rules.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <cstdint>

#include "kb/rtos/backend.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace kb::rtos::backend {
namespace {
// The native handles kept in the opaque storage (each conversion is DEV-KBC-001).
SemaphoreHandle_t SemaphoreOf(const detail::SemaphoreStorage& storage) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<SemaphoreHandle_t>(storage.handle);
}
QueueHandle_t QueueOf(const detail::QueueStorage& storage) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<QueueHandle_t>(storage.handle);
}
EventGroupHandle_t GroupOf(const detail::EventFlagsStorage& storage) noexcept {
  // codeql::misra_deviation_next_line(DEV-KBC-001)
  return static_cast<EventGroupHandle_t>(storage.handle);
}
} // namespace

bool SemaphoreGiveFromIsr(const detail::SemaphoreStorage& storage) noexcept {
  BaseType_t woken = pdFALSE;
  const bool given = xSemaphoreGiveFromISR(SemaphoreOf(storage), &woken) == pdTRUE;
  portYIELD_FROM_ISR(woken);
  return given;
}

bool QueueSendFromIsr(const detail::QueueStorage& storage, const void* item) noexcept {
  BaseType_t woken = pdFALSE;
  const bool sent = xQueueSendFromISR(QueueOf(storage), item, &woken) == pdTRUE;
  portYIELD_FROM_ISR(woken);
  return sent;
}

bool EventFlagsSetFromIsr(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  BaseType_t woken = pdFALSE;
  const bool queued =
      xEventGroupSetBitsFromISR(GroupOf(storage), static_cast<EventBits_t>(bits), &woken) == pdPASS;
  portYIELD_FROM_ISR(woken);
  return queued;
}

} // namespace kb::rtos::backend
