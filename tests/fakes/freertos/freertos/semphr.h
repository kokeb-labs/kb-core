// Fake FreeRTOS semaphore API (see FreeRTOS.h). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_SEMPHR_H
#define KB_FAKE_FREERTOS_SEMPHR_H
#include "freertos/queue.h"
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buffer);
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max, UBaseType_t initial, StaticSemaphore_t* buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t* woken);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* buffer);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex);
#endif
