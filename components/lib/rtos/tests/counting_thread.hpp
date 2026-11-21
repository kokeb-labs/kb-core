// A CRTP thread for the tests: runs `steps` steps (or until stopped), records the hooks it saw.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_RTOS_TEST_COUNTING_THREAD_HPP
#define KB_RTOS_TEST_COUNTING_THREAD_HPP

#include <atomic>
#include <cstdint>

#include "kb/rtos/thread.hpp"

class CountingThread : public kb::rtos::Thread<CountingThread, 4096U> {
public:
  bool Setup() noexcept {
    setups.fetch_add(1U);
    return setup_ok;
  }
  bool Step() noexcept {
    const uint32_t n = steps_done.fetch_add(1U) + 1U;
    if (step_ticks > 0U) {
      kb::rtos::backend::SleepTicks(step_ticks);
    }
    if ((stop_after != 0U) && (n >= stop_after)) {
      RequestStop();
    }
    return (limit == 0U) || (n < limit);
  }
  void Cleanup() noexcept {
    cleanups.fetch_add(1U);
  }

  bool setup_ok{true};
  uint32_t limit{0U};
  uint32_t step_ticks{0U};
  uint32_t stop_after{0U};
  std::atomic<uint32_t> setups{0U};
  std::atomic<uint32_t> steps_done{0U};
  std::atomic<uint32_t> cleanups{0U};
};

#endif
