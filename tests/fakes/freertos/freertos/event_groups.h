// Fake FreeRTOS event group API (see FreeRTOS.h). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_EVENT_GROUPS_H
#define KB_FAKE_FREERTOS_EVENT_GROUPS_H
#include "freertos/FreeRTOS.h"
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* buffer);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t group, EventBits_t bits, BaseType_t* woken);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);
void vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_all, TickType_t ticks);
#endif
