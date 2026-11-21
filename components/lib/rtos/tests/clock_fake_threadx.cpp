// The test clock over the fake ThreadX kernel. SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "fake_threadx.hpp"
#include "kb/rtos/backend.hpp"
#include "rtos_test_clock.hpp"

namespace kb_rtos_test {
void ResetBackend() {
  kb_fake_threadx::Reset();
}
void Advance(uint32_t ticks) {
  kb_fake_threadx::AdvanceTicks(ticks);
}
uint32_t Tick() {
  return kb::rtos::backend::TickCount();
}
} // namespace kb_rtos_test
