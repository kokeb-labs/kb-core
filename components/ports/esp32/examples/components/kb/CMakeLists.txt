# The ESP-IDF component `kb`: ESP-IDF names a component after its folder, so this folder carries the name and
# forwards to kb-core's root CMakeLists.txt, which registers the component under ESP_PLATFORM.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
get_filename_component(KB_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
include("${KB_CORE_ROOT}/CMakeLists.txt")
