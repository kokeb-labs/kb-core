// The test clock over the none backend. SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/rtos/backend.hpp"
#include "kb/rtos/none/control.hpp"
#include "rtos_test_clock.hpp"

namespace kb_rtos_test {
void ResetBackend() {
  kb::rtos::none::SetTicks(0U);
}
void Advance(uint32_t ticks) {
  kb::rtos::none::AdvanceTicks(ticks);
}
uint32_t Tick() {
  return kb::rtos::backend::TickCount();
}
} // namespace kb_rtos_test
