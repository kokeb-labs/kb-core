# A minimal port, built the way a real one consumes kb-core: from the extracted port SDK (docs/port-sdk.md), by
# add_subdirectory, with nothing else from the kb-core repository. KB_SDK_DIR is the extracted folder.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
cmake_minimum_required(VERSION 3.24)
project(minimal_port LANGUAGES CXX)

if(NOT KB_SDK_DIR OR NOT EXISTS "${KB_SDK_DIR}/SDK-MANIFEST.json")
  message(FATAL_ERROR "set KB_SDK_DIR to the extracted kb-port-sdk folder")
endif()
set(KB_PARTS utils rtos dma interfaces CACHE STRING "kb-core parts for the port")
set(KB_QUIET ON)
add_subdirectory("${KB_SDK_DIR}" "${CMAKE_BINARY_DIR}/kb-core" EXCLUDE_FROM_ALL SYSTEM)

add_executable(minimal_port main.cpp "${KB_SDK_DIR}/tests/harness/kb_test_main.cpp")
target_compile_features(minimal_port PRIVATE cxx_std_20)
target_compile_options(minimal_port PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
target_include_directories(minimal_port PRIVATE "${KB_SDK_DIR}/tests/harness")
target_link_libraries(minimal_port PRIVATE kb::interfaces kb::interfaces_host kb::dma kb::dma_host)
