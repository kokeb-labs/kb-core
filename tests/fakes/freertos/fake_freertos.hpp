// Control of the fake FreeRTOS kernel (tests/fakes/freertos/): its tick clock, created tasks and
// timers, failure injection and call counters. Test code only. SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_CONTROL_HPP
#define KB_FAKE_FREERTOS_CONTROL_HPP

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

namespace kb_fake_freertos {

/// What a created task was given.
struct TaskRecord {
  TaskFunction_t code{nullptr};
  void* parameters{nullptr};
  uint32_t depth{0U};
  UBaseType_t priority{0U};
  const char* name{nullptr};
};

void Reset();                       ///< Forgets every object and counter; tick 0.
void SetTick(TickType_t tick);      ///< Sets the tick counter (no timers fire).
void AdvanceTicks(uint32_t ticks);  ///< Advances the clock tick by tick, firing due timers.
void FailNextCreate();              ///< The next ...CreateStatic returns NULL.
std::size_t TaskCount();            ///< Tasks created since Reset().
TaskRecord Task(std::size_t index); ///< What task `index` was created with.
void RunTask(
    std::size_t index);   ///< Runs task `index`'s function to its end, on the caller's stack.
uint32_t TaskDeletes();   ///< vTaskDelete(NULL) calls.
uint32_t Yields();        ///< taskYIELD() calls.
uint32_t YieldsFromIsr(); ///< portYIELD_FROM_ISR(pdTRUE) calls.
int CriticalDepth();      ///< Current critical-section depth.
void WakeHigherPriorityFromIsr(bool woken); ///< Whether the next ISR call reports a woken task.
TickType_t LastTimerCommandTicks();         ///< Block time of the last xTimerStart/xTimerStop.
TickType_t TimerPeriod(std::size_t index);  ///< The period timer `index` was created with.
void SetStackHighWater(UBaseType_t words);  ///< What uxTaskGetStackHighWaterMark returns.

} // namespace kb_fake_freertos

#endif
