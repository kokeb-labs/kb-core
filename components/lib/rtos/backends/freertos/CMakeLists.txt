# The FreeRTOS backend's boundary files (deviations/boundary-files.yml). FreeRTOS headers come in with -isystem:
#   under KB_RTOS=FREERTOS off ESP-IDF   KB_FREERTOS_INCLUDE_DIRS (a kernel with the freertos/<header>.h layout,
#                                        including FreeRTOSConfig.h and the port)
#   with KB_BUILD_TESTS                  kb_rtos_backend_freertos_fake, over the fake kernel in tests/fakes/freertos
# Under ESP-IDF the root CMakeLists.txt compiles these files into the kb component instead.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set(_kb_freertos_sources freertos_boundary.cpp freertos_isr.cpp)
if(KB_RTOS STREQUAL "FREERTOS")
  if(NOT KB_FREERTOS_INCLUDE_DIRS)
    message(FATAL_ERROR "[kb] KB_RTOS=FREERTOS outside ESP-IDF needs KB_FREERTOS_INCLUDE_DIRS (the kernel's include "
                        "directories, with FreeRTOSConfig.h and the port)")
  endif()
  add_library(kb_rtos_backend_freertos STATIC ${_kb_freertos_sources})
  kb_strict_target(kb_rtos_backend_freertos)
  target_include_directories(kb_rtos_backend_freertos SYSTEM PRIVATE ${KB_FREERTOS_INCLUDE_DIRS})
  target_link_libraries(kb_rtos_backend_freertos PUBLIC kb_rtos_api)
endif()
if(KB_BUILD_TESTS)
  add_library(kb_fake_freertos STATIC "${KB_TESTS_DIR}/fakes/freertos/fake_freertos.cpp")
  kb_strict_target(kb_fake_freertos)
  target_include_directories(kb_fake_freertos SYSTEM PUBLIC "${KB_TESTS_DIR}/fakes/freertos")
  add_library(kb_rtos_backend_freertos_fake STATIC ${_kb_freertos_sources})
  kb_strict_target(kb_rtos_backend_freertos_fake)
  target_link_libraries(kb_rtos_backend_freertos_fake PUBLIC kb_rtos_api kb_fake_freertos)
endif()
