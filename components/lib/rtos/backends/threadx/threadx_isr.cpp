/**
 * @file threadx_isr.cpp
 * @brief The interrupt-safe calls of the ThreadX backend (`...FromIsr`). ThreadX lets an interrupt
 * handler give a semaphore, send to a queue and set event flags with the ordinary calls as long as
 * they do not wait, so each one is the thread-context function with no wait (TX_NO_WAIT). No yield
 * request is needed: the port's interrupt exit (context restore, PendSV on Cortex-M) runs the
 * scheduler when the call made a higher-priority thread ready.
 *
 * Not a boundary file: it includes no ThreadX header (threadx_boundary.cpp holds every call).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <cstdint>

#include "kb/rtos/backend.hpp"

namespace kb::rtos::backend {

bool SemaphoreGiveFromIsr(const detail::SemaphoreStorage& storage) noexcept {
  return SemaphoreGive(storage); // tx_semaphore_ceiling_put never waits
}

bool QueueSendFromIsr(const detail::QueueStorage& storage, const void* item) noexcept {
  return QueueSend(storage, item, 0U); // TX_NO_WAIT
}

bool EventFlagsSetFromIsr(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept {
  EventFlagsSet(storage, bits); // tx_event_flags_set never waits and is not deferred
  return true;
}

} // namespace kb::rtos::backend
