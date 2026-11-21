// A fake of the FreeRTOS API that kb-core's FreeRTOS backend calls, for host link-seam tests of the boundary
// files (components/lib/rtos/backends/freertos/). Single-threaded and deterministic; see fake_freertos.hpp for its control.
// Not FreeRTOS: only the names, types and documented results of the calls kb-core makes.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_H
#define KB_FAKE_FREERTOS_H

#include <cstdint>

using TickType_t = uint32_t;
using BaseType_t = long;
using UBaseType_t = unsigned long;
using StackType_t = uint32_t;
using EventBits_t = uint32_t;
using TaskFunction_t = void (*)(void*);

#define pdFALSE (static_cast<BaseType_t>(0))
#define pdTRUE (static_cast<BaseType_t>(1))
#define pdPASS (pdTRUE)
#define pdFAIL (pdFALSE)
#define configTICK_RATE_HZ 1000U
#define configMAX_PRIORITIES 25U
#define configSUPPORT_STATIC_ALLOCATION 1
#define portMAX_DELAY (static_cast<TickType_t>(0xFFFFFFFFUL))

struct StaticSemaphore_t {
  alignas(8) uint8_t opaque[96];
};
struct StaticQueue_t {
  alignas(8) uint8_t opaque[96];
};
struct StaticEventGroup_t {
  alignas(8) uint8_t opaque[48];
};
struct StaticTimer_t {
  alignas(8) uint8_t opaque[64];
};
struct StaticTask_t {
  alignas(8) uint8_t opaque[160];
};

struct FakeQueue;
struct FakeGroup;
struct FakeTimer;
struct FakeTask;
using QueueHandle_t = FakeQueue*;
using SemaphoreHandle_t = FakeQueue*;
using EventGroupHandle_t = FakeGroup*;
using TimerHandle_t = FakeTimer*;
using TaskHandle_t = FakeTask*;

void FakeYield();
void FakeYieldFromIsr(BaseType_t woken);
void FakeEnterCritical();
void FakeExitCritical();

#define taskYIELD() FakeYield()
#define taskENTER_CRITICAL() FakeEnterCritical()
#define taskEXIT_CRITICAL() FakeExitCritical()
#define portYIELD_FROM_ISR(x) FakeYieldFromIsr(x)

#endif
