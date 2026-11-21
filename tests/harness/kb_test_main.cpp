/**
 * @file kb_test_main.cpp
 * @brief Runs every registered test case; the exit code is non-zero when one fails or none ran.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include "kb_test.hpp"

int main() {
  ::kb_test::Registry& r = ::kb_test::GetRegistry();
  int failed_cases = 0;
  for (int i = 0; i < r.count; ++i) {
    const int before = r.failures;
    static_cast<void>(std::printf("[ RUN  ] %s\n", r.tests[i].name));
    r.tests[i].fn();
    const bool ok = r.failures == before;
    static_cast<void>(std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", r.tests[i].name));
    if (!ok) {
      ++failed_cases;
    }
  }
  static_cast<void>(std::printf("%d test cases, %d failed, %d not registered (table full)\n",
                                r.count, failed_cases, r.overflowed));
  return ((failed_cases == 0) && (r.count > 0) && (r.overflowed == 0)) ? 0 : 1;
}
