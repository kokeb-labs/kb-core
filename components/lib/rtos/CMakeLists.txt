# kb::rtos: depends on kb::utils. The typed API is header-only (kb_rtos_api); KB_RTOS selects the one backend that
# kb::rtos links (components/lib/rtos/README.md):
#   NONE      backends/none/          no scheduler, deterministic (host default)
#   HOST      the host port (paths.yml: host_port): std::thread, PC tests only, outside the MISRA claim
#   FREERTOS  backends/freertos/      the boundary files; ESP-IDF provides the kernel, elsewhere KB_FREERTOS_INCLUDE_DIRS
#   THREADX   backends/threadx/       the boundary file; the kernel from KB_THREADX_INCLUDE_DIRS or the pinned
#                                     third_party/threadx built for KB_THREADX_PORT (backends/threadx/README.md)
# With KB_BUILD_TESTS the none backend, the FreeRTOS and ThreadX boundaries over their fake kernels (tests/fakes/)
# and, on a host build, the std::thread backend are all built, each tested against the same API tests; with
# KB_THREADX_HOST_TESTS also the ThreadX boundary on the real kernel's Linux port.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_part(rtos DEPENDS utils)

add_library(kb_rtos_api INTERFACE)
target_compile_features(kb_rtos_api INTERFACE cxx_std_20)
target_include_directories(kb_rtos_api INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(kb_rtos_api INTERFACE kb::utils)

# Queue slots are whole kernel words only where the kernel moves words (ThreadX: sizeof(ULONG), which the backend
# checks); other backends copy bytes, so their slots stay the item size (backend.hpp, kQueueWordBytes).
if(KB_RTOS STREQUAL "THREADX" OR KB_BUILD_TESTS)
  if(NOT KB_THREADX_QUEUE_WORD_BYTES)
    # The Linux port's ULONG is unsigned long except on x86_64; every MCU port's is 32 bits.
    set(KB_THREADX_QUEUE_WORD_BYTES 4)
    if((KB_THREADX_HOST_TESTS OR KB_THREADX_PORT STREQUAL "linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
      set(KB_THREADX_QUEUE_WORD_BYTES 8)
    endif()
  endif()
  target_compile_definitions(kb_rtos_api INTERFACE "KB_RTOS_QUEUE_WORD_BYTES=${KB_THREADX_QUEUE_WORD_BYTES}")
endif()

add_subdirectory(backends/none)

# The FreeRTOS boundary over the fake kernel: the link-seam test target, and what the MISRA scans compile.
if(KB_BUILD_TESTS OR KB_RTOS STREQUAL "FREERTOS")
  add_subdirectory(backends/freertos)
endif()
if(KB_BUILD_TESTS OR KB_RTOS STREQUAL "THREADX")
  add_subdirectory(backends/threadx)
endif()

string(TOLOWER "${KB_RTOS}" _kb_backend)
if(KB_RTOS STREQUAL "HOST" OR ((KB_BUILD_TESTS OR KB_BUILD_EXAMPLES) AND NOT CMAKE_CROSSCOMPILING))
  add_subdirectory("${KB_HOST_PORT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/host-backend")
endif()
target_link_libraries(kb_rtos INTERFACE kb_rtos_api kb_rtos_backend_${_kb_backend})
install(TARGETS kb_rtos_api kb_rtos_backend_${_kb_backend} EXPORT kb_rtosTargets)

kb_header_checks(rtos _kb_rtos_checks HEADERS
  kb/rtos/backend.hpp kb/rtos/backend_rules.hpp kb/rtos/critical_section.hpp kb/rtos/event_flags.hpp kb/rtos/mutex.hpp kb/rtos/queue.hpp
  kb/rtos/rtos.hpp kb/rtos/semaphore.hpp kb/rtos/thread.hpp kb/rtos/time.hpp kb/rtos/timer.hpp)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES ${_kb_rtos_checks}
  "${CMAKE_CURRENT_SOURCE_DIR}/backends/none/none_backend.cpp")
set_property(GLOBAL APPEND PROPERTY KB_CROSS_LIBS kb_rtos_api)
set_property(GLOBAL APPEND PROPERTY KB_CROSS_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/backends/none/inc")
if(KB_BUILD_TESTS)
  set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/freertos/freertos_boundary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/freertos/freertos_isr.cpp")
  set_property(GLOBAL APPEND PROPERTY KB_CROSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/threadx/threadx_boundary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/threadx/threadx_isr.cpp")
  set_property(GLOBAL APPEND PROPERTY KB_CROSS_SYSTEM_INCLUDES "${KB_TESTS_DIR}/fakes/freertos"
    "${KB_TESTS_DIR}/fakes/threadx")
endif()

if(KB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
