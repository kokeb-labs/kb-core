# Host tests of kb::handlers (components/lib/handlers/tests/README.md): the single-threaded core and the hook bases on the none
# backend (deterministic time), the two access facades with real threads on the host backend (contention, lock
# timeouts, recovery storms), and compile-fail tests for wrong facade, bus and plan combinations.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_unit_test(kb_handlers_tests
  SOURCES test_core.cpp test_health.cpp test_seqlock.cpp test_liveness.cpp test_hooks.cpp test_channel.cpp
          test_facades.cpp test_hold.cpp test_async_poller.cpp test_health_journal.cpp
  LIBS kb_handlers_api kb_interfaces_host kb_rtos_backend_none)
foreach(t IN ITEMS kb_handlers_tests kb_handlers_tests_o2)
  target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
if(TARGET kb_rtos_backend_host)
  kb_add_unit_test(kb_handlers_threads SOURCES test_threads.cpp
    LIBS kb_handlers_api kb_interfaces_host kb_rtos_backend_host)
  foreach(t IN ITEMS kb_handlers_threads kb_handlers_threads_o2)
    target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  # Coverage builds: the threads share the instrumented code, so its counters must be updated atomically (plain
  # counters lose counts under contention and gcov then derives negative ones, which fails the coverage report).
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND "${CMAKE_CXX_FLAGS}" MATCHES "--coverage|-fprofile-arcs")
    target_compile_options(kb_handlers_threads PRIVATE -fprofile-update=atomic)
  endif()
endif()
kb_add_compile_fail_tests(handlers "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
  SOURCES guarded_on_owner_bus.cpp owner_access_on_guarded_bus.cpp guarded_requires_owner.cpp wrong_bus_kind.cpp
          recover_without_key.cpp plan_out_of_order.cpp bus_hold_copied.cpp bus_hold_discarded.cpp
          bus_hold_temporary.cpp owner_held_other_bus.cpp guarded_held_from_bus_hold.cpp
          async_hot_path_blocking.cpp
  LIBS kb_handlers_api kb_interfaces_host kb_rtos_backend_none)
add_library(kb_handlers_header_checks OBJECT ${_kb_handlers_checks} ${_kb_binding_checks})
kb_strict_target(kb_handlers_header_checks)
target_link_libraries(kb_handlers_header_checks PRIVATE kb_handlers_api)
foreach(_name IN LISTS KB_HANDLERS_BOUND)
  target_link_libraries(kb_handlers_header_checks PRIVATE ${_name}::${_name})
endforeach()
