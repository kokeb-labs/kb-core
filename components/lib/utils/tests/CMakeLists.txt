# Host unit tests and compile-fail tests of kb::utils (components/lib/utils/tests/README.md).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_unit_test(kb_utils_tests
  SOURCES test_containers.cpp test_crc.cpp test_endian.cpp test_fault_journal.cpp test_mutation_kills.cpp test_numeric.cpp test_span_string.cpp
          test_status_result.cpp test_time.cpp
  LIBS kb::utils)
kb_add_compile_fail_tests(utils "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
  SOURCES checked_add_bool.cpp endian_short_span.cpp fixed_string_zero.cpp instant_ordered.cpp result_ignored.cpp
          result_reference.cpp ring_buffer_zero.cpp spsc_not_power_of_two.cpp static_vector_non_trivial.cpp
          static_vector_zero.cpp time_implicit_count.cpp time_units_mixed.cpp
  LIBS kb::utils)
# The header checks also build on the host, so a header that is not self-contained fails here first.
add_library(kb_utils_header_checks OBJECT ${_kb_utils_checks})
kb_strict_target(kb_utils_header_checks)
target_link_libraries(kb_utils_header_checks PRIVATE kb::utils)
