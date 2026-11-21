// Fake FreeRTOS queue API (see FreeRTOS.h). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_QUEUE_H
#define KB_FAKE_FREERTOS_QUEUE_H
#include "freertos/FreeRTOS.h"
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t* storage, StaticQueue_t* buffer);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void* item, BaseType_t* woken);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
#endif
