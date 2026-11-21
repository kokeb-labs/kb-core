# Host tests of kb::rtos (components/lib/rtos/tests/README.md): the same single-context contract over the none backend and
# over the FreeRTOS and ThreadX boundaries with their fake kernels, the link seams, and the threaded suite on the
# host backend and (KB_THREADX_HOST_TESTS) on the real ThreadX kernel's Linux port.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_unit_test(kb_rtos_tests_none
  SOURCES test_rtos_single.cpp test_none_backend.cpp test_backend_rules.cpp clock_none.cpp
  LIBS kb_rtos_backend_none)
kb_add_unit_test(kb_rtos_tests_freertos
  SOURCES test_rtos_single.cpp test_freertos_seam.cpp clock_fake_freertos.cpp
  LIBS kb_rtos_backend_freertos_fake)
kb_add_unit_test(kb_rtos_tests_threadx
  SOURCES test_rtos_single.cpp test_threadx_seam.cpp clock_fake_threadx.cpp
  LIBS kb_rtos_backend_threadx_fake)
# The threaded suite on the real kernel (ThreadX Linux port): its own main enters the kernel and runs the cases
# on a ThreadX thread (tests/harness/kb_test_main_threadx.cpp).
if(TARGET kb_rtos_backend_threadx_linux)
  add_executable(kb_rtos_tests_threadx_linux "${KB_ROOT}/tests/harness/kb_test_main_threadx.cpp"
    test_rtos_host.cpp test_threadx_kernel.cpp)
  kb_strict_target(kb_rtos_tests_threadx_linux)
  target_include_directories(kb_rtos_tests_threadx_linux PRIVATE "${KB_ROOT}/tests/harness")
  target_compile_definitions(kb_rtos_tests_threadx_linux PRIVATE ${KB_TEST_DEFINITIONS})
  target_link_libraries(kb_rtos_tests_threadx_linux PRIVATE kb_rtos_backend_threadx_linux threadx)
  # The tick the kernel runs at: the threaded suite's timing checks scale with it.
  get_property(_kb_tick GLOBAL PROPERTY KB_THREADX_TICK_HZ)
  target_compile_definitions(kb_rtos_tests_threadx_linux PRIVATE "KB_TEST_TICK_HZ=${_kb_tick}")
  add_test(NAME kb_rtos_tests_threadx_linux COMMAND kb_rtos_tests_threadx_linux)
  set_tests_properties(kb_rtos_tests_threadx_linux PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
if(TARGET kb_rtos_backend_host)
  kb_add_unit_test(kb_rtos_tests_host SOURCES test_rtos_host.cpp LIBS kb_rtos_backend_host)
  # Coverage builds: several threads run the same instrumented code, so the counters are updated atomically; plain
  # counters lose counts and gcov then derives negative ones (a failed report).
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND "${CMAKE_CXX_FLAGS}" MATCHES "--coverage|-fprofile-arcs")
    target_compile_options(kb_rtos_tests_host PRIVATE -fprofile-update=atomic)
  endif()
endif()
kb_add_compile_fail_tests(rtos "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
  SOURCES queue_not_copyable.cpp queue_zero.cpp sleep_bare_integer.cpp thread_missing_step.cpp mutex_copied.cpp
  LIBS kb_rtos_backend_none)
add_library(kb_rtos_header_checks OBJECT ${_kb_rtos_checks})
kb_strict_target(kb_rtos_header_checks)
target_link_libraries(kb_rtos_header_checks PRIVATE kb_rtos_api)
