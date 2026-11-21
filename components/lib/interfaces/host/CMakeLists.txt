# kb::interfaces_host: the host test port, header-only fakes of every contract (test code, outside the MISRA claim).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
add_library(kb_interfaces_host INTERFACE)
add_library(kb::interfaces_host ALIAS kb_interfaces_host)
target_compile_features(kb_interfaces_host INTERFACE ${KB_CXX_FLOOR_FEATURE})
target_include_directories(kb_interfaces_host INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_interfaces_host INTERFACE kb_interfaces_contracts)
