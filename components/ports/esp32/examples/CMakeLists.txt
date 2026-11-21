# kb-core ESP32 examples: one ESP-IDF project, one app per kb-core part (app_config.yml). Build with the ESP-IDF
# build scripts (cloned into scripts/ by CI, or by hand from kokeb-labs espidf-tools):
#   ./scripts/build_app.sh kb_utils_smoke Debug
# or directly: idf.py -DAPP_TYPE=kb_utils_smoke -DBUILD_TYPE=Debug -DAPP_SOURCE_FILE=kb_utils_smoke.cpp build
# kb-core reaches the build as the ESP-IDF component `kb` (components/kb/ forwards to the repository root).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
cmake_minimum_required(VERSION 3.16)

set(DEFAULT_APP_TYPE "kb_utils_smoke" CACHE STRING "Default app for the requirements phase")
set(DEFAULT_BUILD_TYPE "Debug" CACHE STRING "Default build type for the requirements phase")
set(DEFAULT_SOURCE_FILE "kb_utils_smoke.cpp" CACHE STRING "Default source for the requirements phase")
if(NOT DEFINED APP_TYPE)
  set(APP_TYPE "${DEFAULT_APP_TYPE}")
endif()
if(NOT DEFINED BUILD_TYPE)
  set(BUILD_TYPE "${DEFAULT_BUILD_TYPE}")
endif()
set(APP_TYPE "${APP_TYPE}" CACHE STRING "App type to build" FORCE)
set(BUILD_TYPE "${BUILD_TYPE}" CACHE STRING "Build type (Debug/Release)" FORCE)
message(STATUS "Building app type: ${APP_TYPE}, build type: ${BUILD_TYPE}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kb_esp32_${APP_TYPE})
