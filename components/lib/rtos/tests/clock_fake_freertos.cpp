// The test clock over the fake FreeRTOS kernel. SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "fake_freertos.hpp"
#include "kb/rtos/backend.hpp"
#include "rtos_test_clock.hpp"

namespace kb_rtos_test {
void ResetBackend() {
  kb_fake_freertos::Reset();
}
void Advance(uint32_t ticks) {
  kb_fake_freertos::AdvanceTicks(ticks);
}
uint32_t Tick() {
  return kb::rtos::backend::TickCount();
}
} // namespace kb_rtos_test
