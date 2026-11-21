# The host port (ADR-0007): the `host` RTOS backend (KB_RTOS=HOST, and the multi-threaded host tests): std::thread based, PC only, test code
# outside the MISRA claim (components/ports/host/README.md).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
find_package(Threads REQUIRED)
add_library(kb_rtos_backend_host STATIC src/host_backend.cpp)
add_library(kb::rtos_backend_host ALIAS kb_rtos_backend_host)
kb_strict_target(kb_rtos_backend_host)
target_link_libraries(kb_rtos_backend_host PUBLIC kb_rtos_api Threads::Threads)
