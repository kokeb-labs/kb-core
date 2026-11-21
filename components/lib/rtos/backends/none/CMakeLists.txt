# The `none` RTOS backend (KB_RTOS=NONE): no scheduler, deterministic; the backend of every host unit test above.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
add_library(kb_rtos_backend_none STATIC none_backend.cpp)
add_library(kb::rtos_backend_none ALIAS kb_rtos_backend_none)
kb_strict_target(kb_rtos_backend_none)
target_include_directories(kb_rtos_backend_none PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_rtos_backend_none PUBLIC kb_rtos_api)
