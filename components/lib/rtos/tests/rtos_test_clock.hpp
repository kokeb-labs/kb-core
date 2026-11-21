// The test clock of a single-context backend (none, or FreeRTOS over the fake kernel): each test
// executable links one adapter (clock_none.cpp, clock_fake_freertos.cpp) that maps these onto its
// backend's control. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#ifndef KB_RTOS_TEST_CLOCK_HPP
#define KB_RTOS_TEST_CLOCK_HPP

#include <cstdint>

namespace kb_rtos_test {
void ResetBackend();          ///< Forget timers and objects where the backend can; tick 0.
void Advance(uint32_t ticks); ///< Advance the fake clock, firing due timers.
uint32_t Tick();              ///< The tick counter.
} // namespace kb_rtos_test

#endif
