// Fake FreeRTOS task API (see FreeRTOS.h). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_TASK_H
#define KB_FAKE_FREERTOS_TASK_H
#include "freertos/FreeRTOS.h"
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskDelayUntil(TickType_t* previous_wake, TickType_t increment);
TaskHandle_t xTaskCreateStatic(TaskFunction_t code, const char* name, uint32_t depth, void* parameters,
                               UBaseType_t priority, StackType_t* stack, StaticTask_t* buffer);
void vTaskDelete(TaskHandle_t task);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
#endif
