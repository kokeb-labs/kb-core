/**
 * @file kb_test.hpp
 * @brief The dependency-free test registry and check macros of kb-core's host tests.
 *
 * Built with -fno-exceptions -fno-rtti like target code, so it uses plain function pointers and a
 * fixed table. A test file registers cases with KB_TEST("name") { ... } and checks with KB_CHECK,
 * KB_CHECK_EQ and KB_REQUIRE; tests/harness/kb_test_main.cpp runs them all.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_TEST_HARNESS_HPP
#define KB_TEST_HARNESS_HPP

#include <cstdio>

namespace kb_test {

/// One test function.
using TestFn = void (*)();

/// A registered test case.
struct TestCase {
  const char* name;
  TestFn fn;
};

/// Most test cases one executable can hold.
inline constexpr int kMaxTests = 512;

/// Every registered case and the failure count of the run.
struct Registry {
  TestCase tests[kMaxTests]{};
  int count = 0;
  int failures = 0;
  int overflowed = 0;
};

/// The registry of this executable.
inline Registry& GetRegistry() {
  static Registry registry{};
  return registry;
}

/// Adds a case at static initialisation.
struct Registrar {
  Registrar(const char* name, TestFn fn) {
    Registry& r = GetRegistry();
    if (r.count < kMaxTests) {
      r.tests[r.count] = TestCase{name, fn};
      ++r.count;
    } else {
      ++r.overflowed;
    }
  }
};

/// Records a failed check.
inline void ReportFailure(const char* file, int line, const char* what) {
  ++GetRegistry().failures;
  static_cast<void>(std::printf("    FAIL %s:%d: %s\n", file, line, what));
}

/// Records a failed equality check with both values.
inline void ReportFailureValues(const char* file, int line, const char* what, long long a,
                                long long b) {
  ++GetRegistry().failures;
  static_cast<void>(std::printf("    FAIL %s:%d: %s (%lld vs %lld)\n", file, line, what, a, b));
}

/**
 * A value the compiler cannot see through: tests pass their inputs through Rt() so that constexpr
 * functions run at run time (and are measured by coverage) instead of being folded at compile time.
 */
template <typename T>
T Rt(T value) {
  volatile T copy = value;
  return copy;
}

} // namespace kb_test

#define KB_TEST_CONCAT2(a, b) a##b
#define KB_TEST_CONCAT(a, b) KB_TEST_CONCAT2(a, b)

/// Defines a test case.
#define KB_TEST(name)                                                                              \
  static void KB_TEST_CONCAT(kb_test_fn_, __LINE__)();                                             \
  static const ::kb_test::Registrar KB_TEST_CONCAT(kb_test_reg_, __LINE__)(                        \
      name, &KB_TEST_CONCAT(kb_test_fn_, __LINE__));                                               \
  static void KB_TEST_CONCAT(kb_test_fn_, __LINE__)()

/// Non-fatal check.
#define KB_CHECK(cond)                                                                             \
  do {                                                                                             \
    const bool kb_test_ok = static_cast<bool>(cond);                                               \
    if (!kb_test_ok) {                                                                             \
      ::kb_test::ReportFailure(__FILE__, __LINE__, #cond);                                         \
    }                                                                                              \
  } while (false)

/// Non-fatal equality check of two integral or enumeration values.
#define KB_CHECK_EQ(a, b)                                                                          \
  do {                                                                                             \
    const auto kb_test_a = (a);                                                                    \
    const auto kb_test_b = (b);                                                                    \
    if (!(kb_test_a == kb_test_b)) {                                                               \
      ::kb_test::ReportFailureValues(__FILE__, __LINE__, #a " == " #b,                             \
                                     static_cast<long long>(kb_test_a),                            \
                                     static_cast<long long>(kb_test_b));                           \
    }                                                                                              \
  } while (false)

/// Fatal check: ends the current test case.
#define KB_REQUIRE(cond)                                                                           \
  do {                                                                                             \
    if (!static_cast<bool>(cond)) {                                                                \
      ::kb_test::ReportFailure(__FILE__, __LINE__, #cond);                                         \
      return;                                                                                      \
    }                                                                                              \
  } while (false)

#endif // KB_TEST_HARNESS_HPP
