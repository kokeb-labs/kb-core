# examples/host/rtos_thread: see README.md. Built with KB_BUILD_EXAMPLES and run as the ctest test example_rtos_thread.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
# The std::thread backend: threads, queues and timers run for real (the default NONE backend has no scheduler).
kb_add_example(rtos_thread REQUIRES kb_rtos kb_rtos_backend_host LIBS kb_rtos_api kb_rtos_backend_host kb::utils)
