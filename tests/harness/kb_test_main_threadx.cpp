/**
 * @file kb_test_main_threadx.cpp
 * @brief Runs every registered test case on a ThreadX thread of the real kernel (the Linux port):
 * main() enters the kernel, tx_application_define() creates the runner, the runner runs the cases
 * and ends the process with the harness's exit code. Test code; a boundary file because it is the
 * kernel's start-up (deviations/boundary-files.yml).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "kb_test.hpp"

#include "tx_api.h"

namespace {

constexpr std::size_t kRunnerStack = 64U * 1024U;
constexpr UINT kRunnerPriority = 16U; // kb priority 15: above the default kb thread (1, ThreadX 30)

TX_THREAD g_runner{};
alignas(16) std::array<std::byte, kRunnerStack> g_runner_stack{};
std::array<CHAR, 7> g_runner_name{'r', 'u', 'n', 'n', 'e', 'r', '\0'};

void RunAll(ULONG input) {
  static_cast<void>(input);
  ::kb_test::Registry& r = ::kb_test::GetRegistry();
  int failed_cases = 0;
  for (int i = 0; i < r.count; ++i) {
    const int before = r.failures;
    static_cast<void>(std::printf("[ RUN  ] %s\n", r.tests[i].name));
    static_cast<void>(std::fflush(stdout));
    r.tests[i].fn();
    const bool ok = r.failures == before;
    static_cast<void>(std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", r.tests[i].name));
    if (!ok) {
      ++failed_cases;
    }
  }
  static_cast<void>(std::printf("%d test cases, %d failed, %d not registered (table full)\n",
                                r.count, failed_cases, r.overflowed));
  static_cast<void>(std::fflush(stdout));
  // End here: the kernel never returns to main(), and static kb threads must not be released after
  // it.
  std::_Exit(((failed_cases == 0) && (r.count > 0) && (r.overflowed == 0)) ? 0 : 1);
}

} // namespace

extern "C" void tx_application_define(void* first_unused_memory) {
  static_cast<void>(first_unused_memory);
  const UINT result = tx_thread_create(
      &g_runner, g_runner_name.data(), &RunAll, 0U, g_runner_stack.data(), g_runner_stack.size(),
      kRunnerPriority, kRunnerPriority, TX_NO_TIME_SLICE, TX_AUTO_START);
  if (result != TX_SUCCESS) {
    static_cast<void>(std::printf("cannot create the test runner thread: %u\n", result));
    std::_Exit(2);
  }
}

int main() {
  tx_kernel_enter();
  return 3; // not reached
}
