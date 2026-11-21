// Fake FreeRTOS software timer API (see FreeRTOS.h). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_FREERTOS_TIMERS_H
#define KB_FAKE_FREERTOS_TIMERS_H
#include "freertos/FreeRTOS.h"
using TimerCallbackFunction_t = void (*)(TimerHandle_t);
TimerHandle_t xTimerCreateStatic(const char* name, TickType_t period, BaseType_t auto_reload, void* id,
                                 TimerCallbackFunction_t callback, StaticTimer_t* buffer);
void* pvTimerGetTimerID(TimerHandle_t timer);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks);
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks);
#endif
